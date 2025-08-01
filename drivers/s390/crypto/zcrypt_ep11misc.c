// SPDX-License-Identifier: GPL-2.0+
/*
 *  Copyright IBM Corp. 2019
 *  Author(s): Harald Freudenberger <freude@linux.ibm.com>
 *
 *  Collection of EP11 misc functions used by zcrypt and pkey
 */

#define KMSG_COMPONENT "zcrypt"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/export.h>
#include <linux/init.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <asm/zcrypt.h>
#include <asm/pkey.h>
#include <crypto/aes.h>

#include "ap_bus.h"
#include "zcrypt_api.h"
#include "zcrypt_debug.h"
#include "zcrypt_msgtype6.h"
#include "zcrypt_ep11misc.h"
#include "zcrypt_ccamisc.h"

#define EP11_PINBLOB_V1_BYTES 56

/* default iv used here */
static const u8 def_iv[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
			       0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

/*
 * Cprb memory pool held for urgent cases where no memory
 * can be allocated via kmalloc. This pool is only used when
 * alloc_cprbmem() is called with the xflag ZCRYPT_XFLAG_NOMEMALLOC.
 */
#define CPRB_MEMPOOL_ITEM_SIZE (8 * 1024)
static mempool_t *cprb_mempool;

/*
 * This is a pre-allocated memory for the device status array
 * used within the ep11_findcard2() function. It is currently
 * 128 * 128 * 4 bytes = 64 KB big. Usage of this memory is
 * controlled via dev_status_mem_mutex. Needs adaption if more
 * than 128 cards or domains to be are supported.
 */
#define ZCRYPT_DEV_STATUS_CARD_MAX 128
#define ZCRYPT_DEV_STATUS_QUEUE_MAX 128
#define ZCRYPT_DEV_STATUS_ENTRIES (ZCRYPT_DEV_STATUS_CARD_MAX * \
				   ZCRYPT_DEV_STATUS_QUEUE_MAX)
#define ZCRYPT_DEV_STATUS_EXT_SIZE (ZCRYPT_DEV_STATUS_ENTRIES * \
		sizeof(struct zcrypt_device_status_ext))
static void *dev_status_mem;
static DEFINE_MUTEX(dev_status_mem_mutex);

static int ep11_kb_split(const u8 *kb, size_t kblen, u32 kbver,
			 struct ep11kblob_header **kbhdr, size_t *kbhdrsize,
			 u8 **kbpl, size_t *kbplsize)
{
	struct ep11kblob_header *hdr = NULL;
	size_t hdrsize, plsize = 0;
	int rc = -EINVAL;
	u8 *pl = NULL;

	if (kblen < sizeof(struct ep11kblob_header))
		goto out;
	hdr = (struct ep11kblob_header *)kb;

	switch (kbver) {
	case TOKVER_EP11_AES:
		/* header overlays the payload */
		hdrsize = 0;
		break;
	case TOKVER_EP11_ECC_WITH_HEADER:
	case TOKVER_EP11_AES_WITH_HEADER:
		/* payload starts after the header */
		hdrsize = sizeof(struct ep11kblob_header);
		break;
	default:
		goto out;
	}

	plsize = kblen - hdrsize;
	pl = (u8 *)kb + hdrsize;

	if (kbhdr)
		*kbhdr = hdr;
	if (kbhdrsize)
		*kbhdrsize = hdrsize;
	if (kbpl)
		*kbpl = pl;
	if (kbplsize)
		*kbplsize = plsize;

	rc = 0;
out:
	return rc;
}

static int ep11_kb_decode(const u8 *kb, size_t kblen,
			  struct ep11kblob_header **kbhdr, size_t *kbhdrsize,
			  struct ep11keyblob **kbpl, size_t *kbplsize)
{
	struct ep11kblob_header *tmph, *hdr = NULL;
	size_t hdrsize = 0, plsize = 0;
	struct ep11keyblob *pl = NULL;
	int rc = -EINVAL;
	u8 *tmpp;

	if (kblen < sizeof(struct ep11kblob_header))
		goto out;
	tmph = (struct ep11kblob_header *)kb;

	if (tmph->type != TOKTYPE_NON_CCA &&
	    tmph->len > kblen)
		goto out;

	if (ep11_kb_split(kb, kblen, tmph->version,
			  &hdr, &hdrsize, &tmpp, &plsize))
		goto out;

	if (plsize < sizeof(struct ep11keyblob))
		goto out;

	if (!is_ep11_keyblob(tmpp))
		goto out;

	pl = (struct ep11keyblob *)tmpp;
	plsize = hdr->len - hdrsize;

	if (kbhdr)
		*kbhdr = hdr;
	if (kbhdrsize)
		*kbhdrsize = hdrsize;
	if (kbpl)
		*kbpl = pl;
	if (kbplsize)
		*kbplsize = plsize;

	rc = 0;
out:
	return rc;
}

/*
 * For valid ep11 keyblobs, returns a reference to the wrappingkey verification
 * pattern. Otherwise NULL.
 */
const u8 *ep11_kb_wkvp(const u8 *keyblob, u32 keybloblen)
{
	struct ep11keyblob *kb;

	if (ep11_kb_decode(keyblob, keybloblen, NULL, NULL, &kb, NULL))
		return NULL;
	return kb->wkvp;
}
EXPORT_SYMBOL(ep11_kb_wkvp);

/*
 * Simple check if the key blob is a valid EP11 AES key blob with header.
 */
int ep11_check_aes_key_with_hdr(debug_info_t *dbg, int dbflvl,
				const u8 *key, u32 keylen, int checkcpacfexp)
{
	struct ep11kblob_header *hdr = (struct ep11kblob_header *)key;
	struct ep11keyblob *kb = (struct ep11keyblob *)(key + sizeof(*hdr));

#define DBF(...) debug_sprintf_event(dbg, dbflvl, ##__VA_ARGS__)

	if (keylen < sizeof(*hdr) + sizeof(*kb)) {
		DBF("%s key check failed, keylen %u < %zu\n",
		    __func__, keylen, sizeof(*hdr) + sizeof(*kb));
		return -EINVAL;
	}

	if (hdr->type != TOKTYPE_NON_CCA) {
		if (dbg)
			DBF("%s key check failed, type 0x%02x != 0x%02x\n",
			    __func__, (int)hdr->type, TOKTYPE_NON_CCA);
		return -EINVAL;
	}
	if (hdr->hver != 0x00) {
		if (dbg)
			DBF("%s key check failed, header version 0x%02x != 0x00\n",
			    __func__, (int)hdr->hver);
		return -EINVAL;
	}
	if (hdr->version != TOKVER_EP11_AES_WITH_HEADER) {
		if (dbg)
			DBF("%s key check failed, version 0x%02x != 0x%02x\n",
			    __func__, (int)hdr->version, TOKVER_EP11_AES_WITH_HEADER);
		return -EINVAL;
	}
	if (hdr->len > keylen) {
		if (dbg)
			DBF("%s key check failed, header len %d keylen %u mismatch\n",
			    __func__, (int)hdr->len, keylen);
		return -EINVAL;
	}
	if (hdr->len < sizeof(*hdr) + sizeof(*kb)) {
		if (dbg)
			DBF("%s key check failed, header len %d < %zu\n",
			    __func__, (int)hdr->len, sizeof(*hdr) + sizeof(*kb));
		return -EINVAL;
	}

	if (kb->version != EP11_STRUCT_MAGIC) {
		if (dbg)
			DBF("%s key check failed, blob magic 0x%04x != 0x%04x\n",
			    __func__, (int)kb->version, EP11_STRUCT_MAGIC);
		return -EINVAL;
	}
	if (checkcpacfexp && !(kb->attr & EP11_BLOB_PKEY_EXTRACTABLE)) {
		if (dbg)
			DBF("%s key check failed, PKEY_EXTRACTABLE is off\n",
			    __func__);
		return -EINVAL;
	}

#undef DBF

	return 0;
}
EXPORT_SYMBOL(ep11_check_aes_key_with_hdr);

/*
 * Simple check if the key blob is a valid EP11 ECC key blob with header.
 */
int ep11_check_ecc_key_with_hdr(debug_info_t *dbg, int dbflvl,
				const u8 *key, u32 keylen, int checkcpacfexp)
{
	struct ep11kblob_header *hdr = (struct ep11kblob_header *)key;
	struct ep11keyblob *kb = (struct ep11keyblob *)(key + sizeof(*hdr));

#define DBF(...) debug_sprintf_event(dbg, dbflvl, ##__VA_ARGS__)

	if (keylen < sizeof(*hdr) + sizeof(*kb)) {
		DBF("%s key check failed, keylen %u < %zu\n",
		    __func__, keylen, sizeof(*hdr) + sizeof(*kb));
		return -EINVAL;
	}

	if (hdr->type != TOKTYPE_NON_CCA) {
		if (dbg)
			DBF("%s key check failed, type 0x%02x != 0x%02x\n",
			    __func__, (int)hdr->type, TOKTYPE_NON_CCA);
		return -EINVAL;
	}
	if (hdr->hver != 0x00) {
		if (dbg)
			DBF("%s key check failed, header version 0x%02x != 0x00\n",
			    __func__, (int)hdr->hver);
		return -EINVAL;
	}
	if (hdr->version != TOKVER_EP11_ECC_WITH_HEADER) {
		if (dbg)
			DBF("%s key check failed, version 0x%02x != 0x%02x\n",
			    __func__, (int)hdr->version, TOKVER_EP11_ECC_WITH_HEADER);
		return -EINVAL;
	}
	if (hdr->len > keylen) {
		if (dbg)
			DBF("%s key check failed, header len %d keylen %u mismatch\n",
			    __func__, (int)hdr->len, keylen);
		return -EINVAL;
	}
	if (hdr->len < sizeof(*hdr) + sizeof(*kb)) {
		if (dbg)
			DBF("%s key check failed, header len %d < %zu\n",
			    __func__, (int)hdr->len, sizeof(*hdr) + sizeof(*kb));
		return -EINVAL;
	}

	if (kb->version != EP11_STRUCT_MAGIC) {
		if (dbg)
			DBF("%s key check failed, blob magic 0x%04x != 0x%04x\n",
			    __func__, (int)kb->version, EP11_STRUCT_MAGIC);
		return -EINVAL;
	}
	if (checkcpacfexp && !(kb->attr & EP11_BLOB_PKEY_EXTRACTABLE)) {
		if (dbg)
			DBF("%s key check failed, PKEY_EXTRACTABLE is off\n",
			    __func__);
		return -EINVAL;
	}

#undef DBF

	return 0;
}
EXPORT_SYMBOL(ep11_check_ecc_key_with_hdr);

/*
 * Simple check if the key blob is a valid EP11 AES key blob with
 * the header in the session field (old style EP11 AES key).
 */
int ep11_check_aes_key(debug_info_t *dbg, int dbflvl,
		       const u8 *key, u32 keylen, int checkcpacfexp)
{
	struct ep11keyblob *kb = (struct ep11keyblob *)key;

#define DBF(...) debug_sprintf_event(dbg, dbflvl, ##__VA_ARGS__)

	if (keylen < sizeof(*kb)) {
		DBF("%s key check failed, keylen %u < %zu\n",
		    __func__, keylen, sizeof(*kb));
		return -EINVAL;
	}

	if (kb->head.type != TOKTYPE_NON_CCA) {
		if (dbg)
			DBF("%s key check failed, type 0x%02x != 0x%02x\n",
			    __func__, (int)kb->head.type, TOKTYPE_NON_CCA);
		return -EINVAL;
	}
	if (kb->head.version != TOKVER_EP11_AES) {
		if (dbg)
			DBF("%s key check failed, version 0x%02x != 0x%02x\n",
			    __func__, (int)kb->head.version, TOKVER_EP11_AES);
		return -EINVAL;
	}
	if (kb->head.len > keylen) {
		if (dbg)
			DBF("%s key check failed, header len %d keylen %u mismatch\n",
			    __func__, (int)kb->head.len, keylen);
		return -EINVAL;
	}
	if (kb->head.len < sizeof(*kb)) {
		if (dbg)
			DBF("%s key check failed, header len %d < %zu\n",
			    __func__, (int)kb->head.len, sizeof(*kb));
		return -EINVAL;
	}

	if (kb->version != EP11_STRUCT_MAGIC) {
		if (dbg)
			DBF("%s key check failed, blob magic 0x%04x != 0x%04x\n",
			    __func__, (int)kb->version, EP11_STRUCT_MAGIC);
		return -EINVAL;
	}
	if (checkcpacfexp && !(kb->attr & EP11_BLOB_PKEY_EXTRACTABLE)) {
		if (dbg)
			DBF("%s key check failed, PKEY_EXTRACTABLE is off\n",
			    __func__);
		return -EINVAL;
	}

#undef DBF

	return 0;
}
EXPORT_SYMBOL(ep11_check_aes_key);

/*
 * Allocate and prepare ep11 cprb plus additional payload.
 */
static void *alloc_cprbmem(size_t payload_len, u32 xflags)
{
	size_t len = sizeof(struct ep11_cprb) + payload_len;
	struct ep11_cprb *cprb = NULL;

	if (xflags & ZCRYPT_XFLAG_NOMEMALLOC) {
		if (len <= CPRB_MEMPOOL_ITEM_SIZE)
			cprb = mempool_alloc_preallocated(cprb_mempool);
	} else {
		cprb = kmalloc(len, GFP_KERNEL);
	}
	if (!cprb)
		return NULL;
	memset(cprb, 0, len);

	cprb->cprb_len = sizeof(struct ep11_cprb);
	cprb->cprb_ver_id = 0x04;
	memcpy(cprb->func_id, "T4", 2);
	cprb->ret_code = 0xFFFFFFFF;
	cprb->payload_len = payload_len;

	return cprb;
}

/*
 * Free ep11 cprb buffer space.
 */
static void free_cprbmem(void *mem, size_t payload_len, bool scrub, u32 xflags)
{
	if (mem && scrub)
		memzero_explicit(mem, sizeof(struct ep11_cprb) + payload_len);

	if (xflags & ZCRYPT_XFLAG_NOMEMALLOC)
		mempool_free(mem, cprb_mempool);
	else
		kfree(mem);
}

/*
 * Some helper functions related to ASN1 encoding.
 * Limited to length info <= 2 byte.
 */

#define ASN1TAGLEN(x) (2 + (x) + ((x) > 127 ? 1 : 0) + ((x) > 255 ? 1 : 0))

static int asn1tag_write(u8 *ptr, u8 tag, const u8 *pvalue, u16 valuelen)
{
	ptr[0] = tag;
	if (valuelen > 255) {
		ptr[1] = 0x82;
		*((u16 *)(ptr + 2)) = valuelen;
		memcpy(ptr + 4, pvalue, valuelen);
		return 4 + valuelen;
	}
	if (valuelen > 127) {
		ptr[1] = 0x81;
		ptr[2] = (u8)valuelen;
		memcpy(ptr + 3, pvalue, valuelen);
		return 3 + valuelen;
	}
	ptr[1] = (u8)valuelen;
	memcpy(ptr + 2, pvalue, valuelen);
	return 2 + valuelen;
}

/* EP11 payload > 127 bytes starts with this struct */
struct pl_head {
	u8  tag;
	u8  lenfmt;
	u16 len;
	u8  func_tag;
	u8  func_len;
	u32 func;
	u8  dom_tag;
	u8  dom_len;
	u32 dom;
} __packed;

/* prep ep11 payload head helper function */
static inline void prep_head(struct pl_head *h,
			     size_t pl_size, int api, int func)
{
	h->tag = 0x30;
	h->lenfmt = 0x82;
	h->len = pl_size - 4;
	h->func_tag = 0x04;
	h->func_len = sizeof(u32);
	h->func = (api << 16) + func;
	h->dom_tag = 0x04;
	h->dom_len = sizeof(u32);
}

/* prep urb helper function */
static inline void prep_urb(struct ep11_urb *u,
			    struct ep11_target_dev *t, int nt,
			    struct ep11_cprb *req, size_t req_len,
			    struct ep11_cprb *rep, size_t rep_len)
{
	memset(u, 0, sizeof(*u));
	u->targets = (u8 __user *)t;
	u->targets_num = nt;
	u->req = (u8 __user *)req;
	u->req_len = req_len;
	u->resp = (u8 __user *)rep;
	u->resp_len = rep_len;
}

/* Check ep11 reply payload, return 0 or suggested errno value. */
static int check_reply_pl(const u8 *pl, const char *func)
{
	int len;
	u32 ret;

	/* start tag */
	if (*pl++ != 0x30) {
		ZCRYPT_DBF_ERR("%s reply start tag mismatch\n", func);
		return -EIO;
	}

	/* payload length format */
	if (*pl < 127) {
		len = *pl;
		pl++;
	} else if (*pl == 0x81) {
		pl++;
		len = *pl;
		pl++;
	} else if (*pl == 0x82) {
		pl++;
		len = *((u16 *)pl);
		pl += 2;
	} else {
		ZCRYPT_DBF_ERR("%s reply start tag lenfmt mismatch 0x%02hhx\n",
			       func, *pl);
		return -EIO;
	}

	/* len should cover at least 3 fields with 32 bit value each */
	if (len < 3 * 6) {
		ZCRYPT_DBF_ERR("%s reply length %d too small\n", func, len);
		return -EIO;
	}

	/* function tag, length and value */
	if (pl[0] != 0x04 || pl[1] != 0x04) {
		ZCRYPT_DBF_ERR("%s function tag or length mismatch\n", func);
		return -EIO;
	}
	pl += 6;

	/* dom tag, length and value */
	if (pl[0] != 0x04 || pl[1] != 0x04) {
		ZCRYPT_DBF_ERR("%s dom tag or length mismatch\n", func);
		return -EIO;
	}
	pl += 6;

	/* return value tag, length and value */
	if (pl[0] != 0x04 || pl[1] != 0x04) {
		ZCRYPT_DBF_ERR("%s return value tag or length mismatch\n",
			       func);
		return -EIO;
	}
	pl += 2;
	ret = *((u32 *)pl);
	if (ret != 0) {
		ZCRYPT_DBF_ERR("%s return value 0x%08x != 0\n", func, ret);
		return -EIO;
	}

	return 0;
}

/* Check ep11 reply cprb, return 0 or suggested errno value. */
static int check_reply_cprb(const struct ep11_cprb *rep, const char *func)
{
	/* check ep11 reply return code field */
	if (rep->ret_code) {
		ZCRYPT_DBF_ERR("%s ep11 reply ret_code=0x%08x\n", __func__,
			       rep->ret_code);
		if (rep->ret_code == 0x000c0003)
			return -EBUSY;
		else
			return -EIO;
	}

	return 0;
}

/*
 * Helper function which does an ep11 query with given query type.
 */
static int ep11_query_info(u16 cardnr, u16 domain, u32 query_type,
			   size_t buflen, u8 *buf, u32 xflags)
{
	struct ep11_info_req_pl {
		struct pl_head head;
		u8  query_type_tag;
		u8  query_type_len;
		u32 query_type;
		u8  query_subtype_tag;
		u8  query_subtype_len;
		u32 query_subtype;
	} __packed * req_pl;
	struct ep11_info_rep_pl {
		struct pl_head head;
		u8  rc_tag;
		u8  rc_len;
		u32 rc;
		u8  data_tag;
		u8  data_lenfmt;
		u16 data_len;
	} __packed * rep_pl;
	struct ep11_cprb *req = NULL, *rep = NULL;
	struct ep11_target_dev target;
	struct ep11_urb urb;
	int api = EP11_API_V1, rc = -ENOMEM;

	/* request cprb and payload */
	req = alloc_cprbmem(sizeof(struct ep11_info_req_pl), xflags);
	if (!req)
		goto out;
	req_pl = (struct ep11_info_req_pl *)(((u8 *)req) + sizeof(*req));
	prep_head(&req_pl->head, sizeof(*req_pl), api, 38); /* get xcp info */
	req_pl->query_type_tag = 0x04;
	req_pl->query_type_len = sizeof(u32);
	req_pl->query_type = query_type;
	req_pl->query_subtype_tag = 0x04;
	req_pl->query_subtype_len = sizeof(u32);

	/* reply cprb and payload */
	rep = alloc_cprbmem(sizeof(struct ep11_info_rep_pl) + buflen, xflags);
	if (!rep)
		goto out;
	rep_pl = (struct ep11_info_rep_pl *)(((u8 *)rep) + sizeof(*rep));

	/* urb and target */
	target.ap_id = cardnr;
	target.dom_id = domain;
	prep_urb(&urb, &target, 1,
		 req, sizeof(*req) + sizeof(*req_pl),
		 rep, sizeof(*rep) + sizeof(*rep_pl) + buflen);

	rc = zcrypt_send_ep11_cprb(&urb, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s zcrypt_send_ep11_cprb(card=%d dom=%d) failed, rc=%d\n",
			       __func__, (int)cardnr, (int)domain, rc);
		goto out;
	}

	/* check ep11 reply cprb */
	rc = check_reply_cprb(rep, __func__);
	if (rc)
		goto out;

	/* check payload */
	rc = check_reply_pl((u8 *)rep_pl, __func__);
	if (rc)
		goto out;
	if (rep_pl->data_tag != 0x04 || rep_pl->data_lenfmt != 0x82) {
		ZCRYPT_DBF_ERR("%s unknown reply data format\n", __func__);
		rc = -EIO;
		goto out;
	}
	if (rep_pl->data_len > buflen) {
		ZCRYPT_DBF_ERR("%s mismatch between reply data len and buffer len\n",
			       __func__);
		rc = -ENOSPC;
		goto out;
	}

	memcpy(buf, ((u8 *)rep_pl) + sizeof(*rep_pl), rep_pl->data_len);

out:
	free_cprbmem(req, 0, false, xflags);
	free_cprbmem(rep, 0, false, xflags);
	return rc;
}

/*
 * Provide information about an EP11 card.
 */
int ep11_get_card_info(u16 card, struct ep11_card_info *info, u32 xflags)
{
	int rc;
	struct ep11_module_query_info {
		u32 API_ord_nr;
		u32 firmware_id;
		u8  FW_major_vers;
		u8  FW_minor_vers;
		u8  CSP_major_vers;
		u8  CSP_minor_vers;
		u8  fwid[32];
		u8  xcp_config_hash[32];
		u8  CSP_config_hash[32];
		u8  serial[16];
		u8  module_date_time[16];
		u64 op_mode;
		u32 PKCS11_flags;
		u32 ext_flags;
		u32 domains;
		u32 sym_state_bytes;
		u32 digest_state_bytes;
		u32 pin_blob_bytes;
		u32 SPKI_bytes;
		u32 priv_key_blob_bytes;
		u32 sym_blob_bytes;
		u32 max_payload_bytes;
		u32 CP_profile_bytes;
		u32 max_CP_index;
	} __packed * pmqi = NULL;

	/* use the cprb mempool to satisfy this short term mem alloc */
	pmqi = (xflags & ZCRYPT_XFLAG_NOMEMALLOC) ?
		mempool_alloc_preallocated(cprb_mempool) :
		mempool_alloc(cprb_mempool, GFP_KERNEL);
	if (!pmqi)
		return -ENOMEM;
	rc = ep11_query_info(card, AUTOSEL_DOM,
			     0x01 /* module info query */,
			     sizeof(*pmqi), (u8 *)pmqi, xflags);
	if (rc)
		goto out;

	memset(info, 0, sizeof(*info));
	info->API_ord_nr = pmqi->API_ord_nr;
	info->FW_version = (pmqi->FW_major_vers << 8) + pmqi->FW_minor_vers;
	memcpy(info->serial, pmqi->serial, sizeof(info->serial));
	info->op_mode = pmqi->op_mode;

out:
	mempool_free(pmqi, cprb_mempool);
	return rc;
}
EXPORT_SYMBOL(ep11_get_card_info);

/*
 * Provide information about a domain within an EP11 card.
 */
int ep11_get_domain_info(u16 card, u16 domain,
			 struct ep11_domain_info *info, u32 xflags)
{
	int rc;
	struct ep11_domain_query_info {
		u32 dom_index;
		u8  cur_WK_VP[32];
		u8  new_WK_VP[32];
		u32 dom_flags;
		u64 op_mode;
	} __packed dom_query_info;

	rc = ep11_query_info(card, domain, 0x03 /* domain info query */,
			     sizeof(dom_query_info), (u8 *)&dom_query_info,
			     xflags);
	if (rc)
		goto out;

	memset(info, 0, sizeof(*info));
	info->cur_wk_state = '0';
	info->new_wk_state = '0';
	if (dom_query_info.dom_flags & 0x10 /* left imprint mode */) {
		if (dom_query_info.dom_flags & 0x02 /* cur wk valid */) {
			info->cur_wk_state = '1';
			memcpy(info->cur_wkvp, dom_query_info.cur_WK_VP, 32);
		}
		if (dom_query_info.dom_flags & 0x04 || /* new wk present */
		    dom_query_info.dom_flags & 0x08 /* new wk committed */) {
			info->new_wk_state =
				dom_query_info.dom_flags & 0x08 ? '2' : '1';
			memcpy(info->new_wkvp, dom_query_info.new_WK_VP, 32);
		}
	}
	info->op_mode = dom_query_info.op_mode;

out:
	return rc;
}
EXPORT_SYMBOL(ep11_get_domain_info);

/*
 * Default EP11 AES key generate attributes, used when no keygenflags given:
 * XCP_BLOB_ENCRYPT | XCP_BLOB_DECRYPT | XCP_BLOB_PROTKEY_EXTRACTABLE
 */
#define KEY_ATTR_DEFAULTS 0x00200c00

static int _ep11_genaeskey(u16 card, u16 domain,
			   u32 keybitsize, u32 keygenflags,
			   u8 *keybuf, size_t *keybufsize, u32 xflags)
{
	struct keygen_req_pl {
		struct pl_head head;
		u8  var_tag;
		u8  var_len;
		u32 var;
		u8  keybytes_tag;
		u8  keybytes_len;
		u32 keybytes;
		u8  mech_tag;
		u8  mech_len;
		u32 mech;
		u8  attr_tag;
		u8  attr_len;
		u32 attr_header;
		u32 attr_bool_mask;
		u32 attr_bool_bits;
		u32 attr_val_len_type;
		u32 attr_val_len_value;
		/* followed by empty pin tag or empty pinblob tag */
	} __packed * req_pl;
	struct keygen_rep_pl {
		struct pl_head head;
		u8  rc_tag;
		u8  rc_len;
		u32 rc;
		u8  data_tag;
		u8  data_lenfmt;
		u16 data_len;
		u8  data[512];
	} __packed * rep_pl;
	struct ep11_cprb *req = NULL, *rep = NULL;
	size_t req_pl_size, pinblob_size = 0;
	struct ep11_target_dev target;
	struct ep11_urb urb;
	int api, rc = -ENOMEM;
	u8 *p;

	switch (keybitsize) {
	case 128:
	case 192:
	case 256:
		break;
	default:
		ZCRYPT_DBF_ERR("%s unknown/unsupported keybitsize %d\n",
			       __func__, keybitsize);
		rc = -EINVAL;
		goto out;
	}

	/* request cprb and payload */
	api = (!keygenflags || keygenflags & 0x00200000) ?
		EP11_API_V4 : EP11_API_V1;
	if (ap_is_se_guest()) {
		/*
		 * genkey within SE environment requires API ordinal 6
		 * with empty pinblob
		 */
		api = EP11_API_V6;
		pinblob_size = EP11_PINBLOB_V1_BYTES;
	}
	req_pl_size = sizeof(struct keygen_req_pl) + ASN1TAGLEN(pinblob_size);
	req = alloc_cprbmem(req_pl_size, xflags);
	if (!req)
		goto out;
	req_pl = (struct keygen_req_pl *)(((u8 *)req) + sizeof(*req));
	prep_head(&req_pl->head, req_pl_size, api, 21); /* GenerateKey */
	req_pl->var_tag = 0x04;
	req_pl->var_len = sizeof(u32);
	req_pl->keybytes_tag = 0x04;
	req_pl->keybytes_len = sizeof(u32);
	req_pl->keybytes = keybitsize / 8;
	req_pl->mech_tag = 0x04;
	req_pl->mech_len = sizeof(u32);
	req_pl->mech = 0x00001080; /* CKM_AES_KEY_GEN */
	req_pl->attr_tag = 0x04;
	req_pl->attr_len = 5 * sizeof(u32);
	req_pl->attr_header = 0x10010000;
	req_pl->attr_bool_mask = keygenflags ? keygenflags : KEY_ATTR_DEFAULTS;
	req_pl->attr_bool_bits = keygenflags ? keygenflags : KEY_ATTR_DEFAULTS;
	req_pl->attr_val_len_type = 0x00000161; /* CKA_VALUE_LEN */
	req_pl->attr_val_len_value = keybitsize / 8;
	p = ((u8 *)req_pl) + sizeof(*req_pl);
	/* pin tag */
	*p++ = 0x04;
	*p++ = pinblob_size;

	/* reply cprb and payload */
	rep = alloc_cprbmem(sizeof(struct keygen_rep_pl), xflags);
	if (!rep)
		goto out;
	rep_pl = (struct keygen_rep_pl *)(((u8 *)rep) + sizeof(*rep));

	/* urb and target */
	target.ap_id = card;
	target.dom_id = domain;
	prep_urb(&urb, &target, 1,
		 req, sizeof(*req) + req_pl_size,
		 rep, sizeof(*rep) + sizeof(*rep_pl));

	rc = zcrypt_send_ep11_cprb(&urb, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s zcrypt_send_ep11_cprb(card=%d dom=%d) failed, rc=%d\n",
			       __func__, (int)card, (int)domain, rc);
		goto out;
	}

	/* check ep11 reply cprb */
	rc = check_reply_cprb(rep, __func__);
	if (rc)
		goto out;

	/* check payload */
	rc = check_reply_pl((u8 *)rep_pl, __func__);
	if (rc)
		goto out;
	if (rep_pl->data_tag != 0x04 || rep_pl->data_lenfmt != 0x82) {
		ZCRYPT_DBF_ERR("%s unknown reply data format\n", __func__);
		rc = -EIO;
		goto out;
	}
	if (rep_pl->data_len > *keybufsize) {
		ZCRYPT_DBF_ERR("%s mismatch reply data len / key buffer len\n",
			       __func__);
		rc = -ENOSPC;
		goto out;
	}

	/* copy key blob */
	memcpy(keybuf, rep_pl->data, rep_pl->data_len);
	*keybufsize = rep_pl->data_len;

out:
	free_cprbmem(req, 0, false, xflags);
	free_cprbmem(rep, sizeof(struct keygen_rep_pl), true, xflags);
	return rc;
}

int ep11_genaeskey(u16 card, u16 domain, u32 keybitsize, u32 keygenflags,
		   u8 *keybuf, u32 *keybufsize, u32 keybufver, u32 xflags)
{
	struct ep11kblob_header *hdr;
	size_t hdr_size, pl_size;
	u8 *pl;
	int rc;

	switch (keybufver) {
	case TOKVER_EP11_AES:
	case TOKVER_EP11_AES_WITH_HEADER:
		break;
	default:
		return -EINVAL;
	}

	rc = ep11_kb_split(keybuf, *keybufsize, keybufver,
			   &hdr, &hdr_size, &pl, &pl_size);
	if (rc)
		return rc;

	rc = _ep11_genaeskey(card, domain, keybitsize, keygenflags,
			     pl, &pl_size, xflags);
	if (rc)
		return rc;

	*keybufsize = hdr_size + pl_size;

	/* update header information */
	hdr->type = TOKTYPE_NON_CCA;
	hdr->len = *keybufsize;
	hdr->version = keybufver;
	hdr->bitlen = keybitsize;

	return 0;
}
EXPORT_SYMBOL(ep11_genaeskey);

static int ep11_cryptsingle(u16 card, u16 domain,
			    u16 mode, u32 mech, const u8 *iv,
			    const u8 *key, size_t keysize,
			    const u8 *inbuf, size_t inbufsize,
			    u8 *outbuf, size_t *outbufsize,
			    u32 xflags)
{
	struct crypt_req_pl {
		struct pl_head head;
		u8  var_tag;
		u8  var_len;
		u32 var;
		u8  mech_tag;
		u8  mech_len;
		u32 mech;
		/*
		 * maybe followed by iv data
		 * followed by key tag + key blob
		 * followed by plaintext tag + plaintext
		 */
	} __packed * req_pl;
	struct crypt_rep_pl {
		struct pl_head head;
		u8  rc_tag;
		u8  rc_len;
		u32 rc;
		u8  data_tag;
		u8  data_lenfmt;
		/* data follows */
	} __packed * rep_pl;
	struct ep11_cprb *req = NULL, *rep = NULL;
	struct ep11_target_dev target;
	struct ep11_urb urb;
	size_t req_pl_size, rep_pl_size = 0;
	int n, api = EP11_API_V1, rc = -ENOMEM;
	u8 *p;

	/* the simple asn1 coding used has length limits */
	if (keysize > 0xFFFF || inbufsize > 0xFFFF)
		return -EINVAL;

	/* request cprb and payload */
	req_pl_size = sizeof(struct crypt_req_pl) + (iv ? 16 : 0)
		+ ASN1TAGLEN(keysize) + ASN1TAGLEN(inbufsize);
	req = alloc_cprbmem(req_pl_size, xflags);
	if (!req)
		goto out;
	req_pl = (struct crypt_req_pl *)(((u8 *)req) + sizeof(*req));
	prep_head(&req_pl->head, req_pl_size, api, (mode ? 20 : 19));
	req_pl->var_tag = 0x04;
	req_pl->var_len = sizeof(u32);
	/* mech is mech + mech params (iv here) */
	req_pl->mech_tag = 0x04;
	req_pl->mech_len = sizeof(u32) + (iv ? 16 : 0);
	req_pl->mech = (mech ? mech : 0x00001085); /* CKM_AES_CBC_PAD */
	p = ((u8 *)req_pl) + sizeof(*req_pl);
	if (iv) {
		memcpy(p, iv, 16);
		p += 16;
	}
	/* key and input data */
	p += asn1tag_write(p, 0x04, key, keysize);
	p += asn1tag_write(p, 0x04, inbuf, inbufsize);

	/* reply cprb and payload, assume out data size <= in data size + 32 */
	rep_pl_size = sizeof(struct crypt_rep_pl) + ASN1TAGLEN(inbufsize + 32);
	rep = alloc_cprbmem(rep_pl_size, xflags);
	if (!rep)
		goto out;
	rep_pl = (struct crypt_rep_pl *)(((u8 *)rep) + sizeof(*rep));

	/* urb and target */
	target.ap_id = card;
	target.dom_id = domain;
	prep_urb(&urb, &target, 1,
		 req, sizeof(*req) + req_pl_size,
		 rep, sizeof(*rep) + rep_pl_size);

	rc = zcrypt_send_ep11_cprb(&urb, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s zcrypt_send_ep11_cprb(card=%d dom=%d) failed, rc=%d\n",
			       __func__, (int)card, (int)domain, rc);
		goto out;
	}

	/* check ep11 reply cprb */
	rc = check_reply_cprb(rep, __func__);
	if (rc)
		goto out;

	/* check payload */
	rc = check_reply_pl((u8 *)rep_pl, __func__);
	if (rc)
		goto out;
	if (rep_pl->data_tag != 0x04) {
		ZCRYPT_DBF_ERR("%s unknown reply data format\n", __func__);
		rc = -EIO;
		goto out;
	}
	p = ((u8 *)rep_pl) + sizeof(*rep_pl);
	if (rep_pl->data_lenfmt <= 127) {
		n = rep_pl->data_lenfmt;
	} else if (rep_pl->data_lenfmt == 0x81) {
		n = *p++;
	} else if (rep_pl->data_lenfmt == 0x82) {
		n = *((u16 *)p);
		p += 2;
	} else {
		ZCRYPT_DBF_ERR("%s unknown reply data length format 0x%02hhx\n",
			       __func__, rep_pl->data_lenfmt);
		rc = -EIO;
		goto out;
	}
	if (n > *outbufsize) {
		ZCRYPT_DBF_ERR("%s mismatch reply data len %d / output buffer %zu\n",
			       __func__, n, *outbufsize);
		rc = -ENOSPC;
		goto out;
	}

	memcpy(outbuf, p, n);
	*outbufsize = n;

out:
	free_cprbmem(req, req_pl_size, true, xflags);
	free_cprbmem(rep, rep_pl_size, true, xflags);
	return rc;
}

static int _ep11_unwrapkey(u16 card, u16 domain,
			   const u8 *kek, size_t keksize,
			   const u8 *enckey, size_t enckeysize,
			   u32 mech, const u8 *iv,
			   u32 keybitsize, u32 keygenflags,
			   u8 *keybuf, size_t *keybufsize, u32 xflags)
{
	struct uw_req_pl {
		struct pl_head head;
		u8  attr_tag;
		u8  attr_len;
		u32 attr_header;
		u32 attr_bool_mask;
		u32 attr_bool_bits;
		u32 attr_key_type;
		u32 attr_key_type_value;
		u32 attr_val_len;
		u32 attr_val_len_value;
		u8  mech_tag;
		u8  mech_len;
		u32 mech;
		/*
		 * maybe followed by iv data
		 * followed by kek tag + kek blob
		 * followed by empty mac tag
		 * followed by empty pin tag or empty pinblob tag
		 * followed by encryted key tag + bytes
		 */
	} __packed * req_pl;
	struct uw_rep_pl {
		struct pl_head head;
		u8  rc_tag;
		u8  rc_len;
		u32 rc;
		u8  data_tag;
		u8  data_lenfmt;
		u16 data_len;
		u8  data[512];
	} __packed * rep_pl;
	struct ep11_cprb *req = NULL, *rep = NULL;
	size_t req_pl_size, pinblob_size = 0;
	struct ep11_target_dev target;
	struct ep11_urb urb;
	int api, rc = -ENOMEM;
	u8 *p;

	/* request cprb and payload */
	api = (!keygenflags || keygenflags & 0x00200000) ?
		EP11_API_V4 : EP11_API_V1;
	if (ap_is_se_guest()) {
		/*
		 * unwrap within SE environment requires API ordinal 6
		 * with empty pinblob
		 */
		api = EP11_API_V6;
		pinblob_size = EP11_PINBLOB_V1_BYTES;
	}
	req_pl_size = sizeof(struct uw_req_pl) + (iv ? 16 : 0)
		+ ASN1TAGLEN(keksize) + ASN1TAGLEN(0)
		+ ASN1TAGLEN(pinblob_size) + ASN1TAGLEN(enckeysize);
	req = alloc_cprbmem(req_pl_size, xflags);
	if (!req)
		goto out;
	req_pl = (struct uw_req_pl *)(((u8 *)req) + sizeof(*req));
	prep_head(&req_pl->head, req_pl_size, api, 34); /* UnwrapKey */
	req_pl->attr_tag = 0x04;
	req_pl->attr_len = 7 * sizeof(u32);
	req_pl->attr_header = 0x10020000;
	req_pl->attr_bool_mask = keygenflags ? keygenflags : KEY_ATTR_DEFAULTS;
	req_pl->attr_bool_bits = keygenflags ? keygenflags : KEY_ATTR_DEFAULTS;
	req_pl->attr_key_type = 0x00000100; /* CKA_KEY_TYPE */
	req_pl->attr_key_type_value = 0x0000001f; /* CKK_AES */
	req_pl->attr_val_len = 0x00000161; /* CKA_VALUE_LEN */
	req_pl->attr_val_len_value = keybitsize / 8;
	/* mech is mech + mech params (iv here) */
	req_pl->mech_tag = 0x04;
	req_pl->mech_len = sizeof(u32) + (iv ? 16 : 0);
	req_pl->mech = (mech ? mech : 0x00001085); /* CKM_AES_CBC_PAD */
	p = ((u8 *)req_pl) + sizeof(*req_pl);
	if (iv) {
		memcpy(p, iv, 16);
		p += 16;
	}
	/* kek */
	p += asn1tag_write(p, 0x04, kek, keksize);
	/* empty mac key tag */
	*p++ = 0x04;
	*p++ = 0;
	/* pin tag */
	*p++ = 0x04;
	*p++ = pinblob_size;
	p += pinblob_size;
	/* encrypted key value tag and bytes */
	p += asn1tag_write(p, 0x04, enckey, enckeysize);

	/* reply cprb and payload */
	rep = alloc_cprbmem(sizeof(struct uw_rep_pl), xflags);
	if (!rep)
		goto out;
	rep_pl = (struct uw_rep_pl *)(((u8 *)rep) + sizeof(*rep));

	/* urb and target */
	target.ap_id = card;
	target.dom_id = domain;
	prep_urb(&urb, &target, 1,
		 req, sizeof(*req) + req_pl_size,
		 rep, sizeof(*rep) + sizeof(*rep_pl));

	rc = zcrypt_send_ep11_cprb(&urb, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s zcrypt_send_ep11_cprb(card=%d dom=%d) failed, rc=%d\n",
			       __func__, (int)card, (int)domain, rc);
		goto out;
	}

	/* check ep11 reply cprb */
	rc = check_reply_cprb(rep, __func__);
	if (rc)
		goto out;

	/* check payload */
	rc = check_reply_pl((u8 *)rep_pl, __func__);
	if (rc)
		goto out;
	if (rep_pl->data_tag != 0x04 || rep_pl->data_lenfmt != 0x82) {
		ZCRYPT_DBF_ERR("%s unknown reply data format\n", __func__);
		rc = -EIO;
		goto out;
	}
	if (rep_pl->data_len > *keybufsize) {
		ZCRYPT_DBF_ERR("%s mismatch reply data len / key buffer len\n",
			       __func__);
		rc = -ENOSPC;
		goto out;
	}

	/* copy key blob */
	memcpy(keybuf, rep_pl->data, rep_pl->data_len);
	*keybufsize = rep_pl->data_len;

out:
	free_cprbmem(req, req_pl_size, true, xflags);
	free_cprbmem(rep, sizeof(struct uw_rep_pl), true, xflags);
	return rc;
}

static int ep11_unwrapkey(u16 card, u16 domain,
			  const u8 *kek, size_t keksize,
			  const u8 *enckey, size_t enckeysize,
			  u32 mech, const u8 *iv,
			  u32 keybitsize, u32 keygenflags,
			  u8 *keybuf, u32 *keybufsize,
			  u8 keybufver, u32 xflags)
{
	struct ep11kblob_header *hdr;
	size_t hdr_size, pl_size;
	u8 *pl;
	int rc;

	rc = ep11_kb_split(keybuf, *keybufsize, keybufver,
			   &hdr, &hdr_size, &pl, &pl_size);
	if (rc)
		return rc;

	rc = _ep11_unwrapkey(card, domain, kek, keksize, enckey, enckeysize,
			     mech, iv, keybitsize, keygenflags,
			     pl, &pl_size, xflags);
	if (rc)
		return rc;

	*keybufsize = hdr_size + pl_size;

	/* update header information */
	hdr = (struct ep11kblob_header *)keybuf;
	hdr->type = TOKTYPE_NON_CCA;
	hdr->len = *keybufsize;
	hdr->version = keybufver;
	hdr->bitlen = keybitsize;

	return 0;
}

static int _ep11_wrapkey(u16 card, u16 domain,
			 const u8 *key, size_t keysize,
			 u32 mech, const u8 *iv,
			 u8 *databuf, size_t *datasize, u32 xflags)
{
	struct wk_req_pl {
		struct pl_head head;
		u8  var_tag;
		u8  var_len;
		u32 var;
		u8  mech_tag;
		u8  mech_len;
		u32 mech;
		/*
		 * followed by iv data
		 * followed by key tag + key blob
		 * followed by dummy kek param
		 * followed by dummy mac param
		 */
	} __packed * req_pl;
	struct wk_rep_pl {
		struct pl_head head;
		u8  rc_tag;
		u8  rc_len;
		u32 rc;
		u8  data_tag;
		u8  data_lenfmt;
		u16 data_len;
		u8  data[1024];
	} __packed * rep_pl;
	struct ep11_cprb *req = NULL, *rep = NULL;
	struct ep11_target_dev target;
	struct ep11_urb urb;
	size_t req_pl_size;
	int api, rc = -ENOMEM;
	u8 *p;

	/* request cprb and payload */
	req_pl_size = sizeof(struct wk_req_pl) + (iv ? 16 : 0)
		+ ASN1TAGLEN(keysize) + 4;
	req = alloc_cprbmem(req_pl_size, xflags);
	if (!req)
		goto out;
	if (!mech || mech == 0x80060001)
		req->flags |= 0x20; /* CPACF_WRAP needs special bit */
	req_pl = (struct wk_req_pl *)(((u8 *)req) + sizeof(*req));
	api = (!mech || mech == 0x80060001) ? /* CKM_IBM_CPACF_WRAP */
		EP11_API_V4 : EP11_API_V1;
	prep_head(&req_pl->head, req_pl_size, api, 33); /* WrapKey */
	req_pl->var_tag = 0x04;
	req_pl->var_len = sizeof(u32);
	/* mech is mech + mech params (iv here) */
	req_pl->mech_tag = 0x04;
	req_pl->mech_len = sizeof(u32) + (iv ? 16 : 0);
	req_pl->mech = (mech ? mech : 0x80060001); /* CKM_IBM_CPACF_WRAP */
	p = ((u8 *)req_pl) + sizeof(*req_pl);
	if (iv) {
		memcpy(p, iv, 16);
		p += 16;
	}
	/* key blob */
	p += asn1tag_write(p, 0x04, key, keysize);
	/* empty kek tag */
	*p++ = 0x04;
	*p++ = 0;
	/* empty mac tag */
	*p++ = 0x04;
	*p++ = 0;

	/* reply cprb and payload */
	rep = alloc_cprbmem(sizeof(struct wk_rep_pl), xflags);
	if (!rep)
		goto out;
	rep_pl = (struct wk_rep_pl *)(((u8 *)rep) + sizeof(*rep));

	/* urb and target */
	target.ap_id = card;
	target.dom_id = domain;
	prep_urb(&urb, &target, 1,
		 req, sizeof(*req) + req_pl_size,
		 rep, sizeof(*rep) + sizeof(*rep_pl));

	rc = zcrypt_send_ep11_cprb(&urb, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s zcrypt_send_ep11_cprb(card=%d dom=%d) failed, rc=%d\n",
			       __func__, (int)card, (int)domain, rc);
		goto out;
	}

	/* check ep11 reply cprb */
	rc = check_reply_cprb(rep, __func__);
	if (rc)
		goto out;

	/* check payload */
	rc = check_reply_pl((u8 *)rep_pl, __func__);
	if (rc)
		goto out;
	if (rep_pl->data_tag != 0x04 || rep_pl->data_lenfmt != 0x82) {
		ZCRYPT_DBF_ERR("%s unknown reply data format\n", __func__);
		rc = -EIO;
		goto out;
	}
	if (rep_pl->data_len > *datasize) {
		ZCRYPT_DBF_ERR("%s mismatch reply data len / data buffer len\n",
			       __func__);
		rc = -ENOSPC;
		goto out;
	}

	/* copy the data from the cprb to the data buffer */
	memcpy(databuf, rep_pl->data, rep_pl->data_len);
	*datasize = rep_pl->data_len;

out:
	free_cprbmem(req, req_pl_size, true, xflags);
	free_cprbmem(rep, sizeof(struct wk_rep_pl), true, xflags);
	return rc;
}

int ep11_clr2keyblob(u16 card, u16 domain, u32 keybitsize, u32 keygenflags,
		     const u8 *clrkey, u8 *keybuf, u32 *keybufsize,
		     u32 keytype, u32 xflags)
{
	int rc;
	void *mem;
	u8 encbuf[64], *kek;
	size_t clrkeylen, keklen, encbuflen = sizeof(encbuf);

	if (keybitsize == 128 || keybitsize == 192 || keybitsize == 256) {
		clrkeylen = keybitsize / 8;
	} else {
		ZCRYPT_DBF_ERR("%s unknown/unsupported keybitsize %d\n",
			       __func__, keybitsize);
		return -EINVAL;
	}

	/*
	 * Allocate space for the temp kek.
	 * Also we only need up to MAXEP11AESKEYBLOBSIZE bytes for this
	 * we use the already existing cprb mempool to solve this
	 * short term memory requirement.
	 */
	mem = (xflags & ZCRYPT_XFLAG_NOMEMALLOC) ?
		mempool_alloc_preallocated(cprb_mempool) :
		mempool_alloc(cprb_mempool, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;
	kek = (u8 *)mem;
	keklen = MAXEP11AESKEYBLOBSIZE;

	/* Step 1: generate AES 256 bit random kek key */
	rc = _ep11_genaeskey(card, domain, 256,
			     0x00006c00, /* EN/DECRYPT, WRAP/UNWRAP */
			     kek, &keklen, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s generate kek key failed, rc=%d\n",
			       __func__, rc);
		goto out;
	}

	/* Step 2: encrypt clear key value with the kek key */
	rc = ep11_cryptsingle(card, domain, 0, 0, def_iv, kek, keklen,
			      clrkey, clrkeylen, encbuf, &encbuflen, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s encrypting key value with kek key failed, rc=%d\n",
			       __func__, rc);
		goto out;
	}

	/* Step 3: import the encrypted key value as a new key */
	rc = ep11_unwrapkey(card, domain, kek, keklen,
			    encbuf, encbuflen, 0, def_iv,
			    keybitsize, 0, keybuf, keybufsize, keytype, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s importing key value as new key failed, rc=%d\n",
			       __func__, rc);
		goto out;
	}

out:
	mempool_free(mem, cprb_mempool);
	return rc;
}
EXPORT_SYMBOL(ep11_clr2keyblob);

int ep11_kblob2protkey(u16 card, u16 dom,
		       const u8 *keyblob, u32 keybloblen,
		       u8 *protkey, u32 *protkeylen, u32 *protkeytype,
		       u32 xflags)
{
	struct ep11kblob_header *hdr;
	struct ep11keyblob *key;
	size_t wkbuflen, keylen;
	struct wk_info {
		u16 version;
		u8  res1[16];
		u32 pkeytype;
		u32 pkeybitsize;
		u64 pkeysize;
		u8  res2[8];
		u8  pkey[];
	} __packed * wki;
	u8 *wkbuf = NULL;
	int rc = -EIO;

	if (ep11_kb_decode((u8 *)keyblob, keybloblen, &hdr, NULL, &key, &keylen))
		return -EINVAL;

	if (hdr->version == TOKVER_EP11_AES) {
		/* wipe overlayed header */
		memset(hdr, 0, sizeof(*hdr));
	}
	/* !!! hdr is no longer a valid header !!! */

	/* need a temp working buffer */
	wkbuflen = (keylen + AES_BLOCK_SIZE) & (~(AES_BLOCK_SIZE - 1));
	if (wkbuflen > CPRB_MEMPOOL_ITEM_SIZE) {
		/* this should never happen */
		rc = -ENOMEM;
		ZCRYPT_DBF_WARN("%s wkbuflen %d > cprb mempool item size %d, rc=%d\n",
				__func__, (int)wkbuflen, CPRB_MEMPOOL_ITEM_SIZE, rc);
		return rc;
	}
	/* use the cprb mempool to satisfy this short term mem allocation */
	wkbuf = (xflags & ZCRYPT_XFLAG_NOMEMALLOC) ?
		mempool_alloc_preallocated(cprb_mempool) :
		mempool_alloc(cprb_mempool, GFP_ATOMIC);
	if (!wkbuf) {
		rc = -ENOMEM;
		ZCRYPT_DBF_WARN("%s allocating tmp buffer via cprb mempool failed, rc=%d\n",
				__func__, rc);
		return rc;
	}

	/* ep11 secure key -> protected key + info */
	rc = _ep11_wrapkey(card, dom, (u8 *)key, keylen,
			   0, def_iv, wkbuf, &wkbuflen, xflags);
	if (rc) {
		ZCRYPT_DBF_ERR("%s rewrapping ep11 key to pkey failed, rc=%d\n",
			       __func__, rc);
		goto out;
	}
	wki = (struct wk_info *)wkbuf;

	/* check struct version and pkey type */
	if (wki->version != 1 || wki->pkeytype < 1 || wki->pkeytype > 5) {
		ZCRYPT_DBF_ERR("%s wk info version %d or pkeytype %d mismatch.\n",
			       __func__, (int)wki->version, (int)wki->pkeytype);
		rc = -EIO;
		goto out;
	}

	/* check protected key type field */
	switch (wki->pkeytype) {
	case 1: /* AES */
		switch (wki->pkeysize) {
		case 16 + 32:
			/* AES 128 protected key */
			if (protkeytype)
				*protkeytype = PKEY_KEYTYPE_AES_128;
			break;
		case 24 + 32:
			/* AES 192 protected key */
			if (protkeytype)
				*protkeytype = PKEY_KEYTYPE_AES_192;
			break;
		case 32 + 32:
			/* AES 256 protected key */
			if (protkeytype)
				*protkeytype = PKEY_KEYTYPE_AES_256;
			break;
		default:
			ZCRYPT_DBF_ERR("%s unknown/unsupported AES pkeysize %d\n",
				       __func__, (int)wki->pkeysize);
			rc = -EIO;
			goto out;
		}
		break;
	case 3: /* EC-P */
	case 4: /* EC-ED */
	case 5: /* EC-BP */
		if (protkeytype)
			*protkeytype = PKEY_KEYTYPE_ECC;
		break;
	case 2: /* TDES */
	default:
		ZCRYPT_DBF_ERR("%s unknown/unsupported key type %d\n",
			       __func__, (int)wki->pkeytype);
		rc = -EIO;
		goto out;
	}

	/* copy the translated protected key */
	if (wki->pkeysize > *protkeylen) {
		ZCRYPT_DBF_ERR("%s wk info pkeysize %llu > protkeysize %u\n",
			       __func__, wki->pkeysize, *protkeylen);
		rc = -EINVAL;
		goto out;
	}
	memcpy(protkey, wki->pkey, wki->pkeysize);
	*protkeylen = wki->pkeysize;

out:
	mempool_free(wkbuf, cprb_mempool);
	return rc;
}
EXPORT_SYMBOL(ep11_kblob2protkey);

int ep11_findcard2(u32 *apqns, u32 *nr_apqns, u16 cardnr, u16 domain,
		   int minhwtype, int minapi, const u8 *wkvp, u32 xflags)
{
	struct zcrypt_device_status_ext *device_status;
	struct ep11_domain_info edi;
	struct ep11_card_info eci;
	u32 _nr_apqns = 0;
	int i, card, dom;

	/* occupy the device status memory */
	mutex_lock(&dev_status_mem_mutex);
	memset(dev_status_mem, 0, ZCRYPT_DEV_STATUS_EXT_SIZE);
	device_status = (struct zcrypt_device_status_ext *)dev_status_mem;

	/* fetch crypto device status into this struct */
	zcrypt_device_status_mask_ext(device_status,
				      ZCRYPT_DEV_STATUS_CARD_MAX,
				      ZCRYPT_DEV_STATUS_QUEUE_MAX);

	/* walk through all the crypto apqnss */
	for (i = 0; i < ZCRYPT_DEV_STATUS_ENTRIES; i++) {
		card = AP_QID_CARD(device_status[i].qid);
		dom = AP_QID_QUEUE(device_status[i].qid);
		/* check online state */
		if (!device_status[i].online)
			continue;
		/* check for ep11 functions */
		if (!(device_status[i].functions & 0x01))
			continue;
		/* check cardnr */
		if (cardnr != 0xFFFF && card != cardnr)
			continue;
		/* check domain */
		if (domain != 0xFFFF && dom != domain)
			continue;
		/* check min hardware type */
		if (minhwtype && device_status[i].hwtype < minhwtype)
			continue;
		/* check min api version if given */
		if (minapi > 0) {
			if (ep11_get_card_info(card, &eci, xflags))
				continue;
			if (minapi > eci.API_ord_nr)
				continue;
		}
		/* check wkvp if given */
		if (wkvp) {
			if (ep11_get_domain_info(card, dom, &edi, xflags))
				continue;
			if (edi.cur_wk_state != '1')
				continue;
			if (memcmp(wkvp, edi.cur_wkvp, 16))
				continue;
		}
		/* apqn passed all filtering criterons, add to the array */
		if (_nr_apqns < *nr_apqns)
			apqns[_nr_apqns++] = (((u16)card) << 16) | ((u16)dom);
	}

	*nr_apqns = _nr_apqns;

	mutex_unlock(&dev_status_mem_mutex);

	return _nr_apqns ? 0 : -ENODEV;
}
EXPORT_SYMBOL(ep11_findcard2);

int __init zcrypt_ep11misc_init(void)
{
	/* Pre-allocate a small memory pool for ep11 cprbs. */
	cprb_mempool = mempool_create_kmalloc_pool(2 * zcrypt_mempool_threshold,
						   CPRB_MEMPOOL_ITEM_SIZE);
	if (!cprb_mempool)
		return -ENOMEM;

	/* Pre-allocate one crypto status card struct used in ep11_findcard2() */
	dev_status_mem = kvmalloc(ZCRYPT_DEV_STATUS_EXT_SIZE, GFP_KERNEL);
	if (!dev_status_mem) {
		mempool_destroy(cprb_mempool);
		return -ENOMEM;
	}

	return 0;
}

void zcrypt_ep11misc_exit(void)
{
	mutex_lock(&dev_status_mem_mutex);
	kvfree(dev_status_mem);
	mutex_unlock(&dev_status_mem_mutex);
	mempool_destroy(cprb_mempool);
}
