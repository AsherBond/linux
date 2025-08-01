// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux Socket Filter - Kernel level socket filtering
 *
 * Based on the design of the Berkeley Packet Filter. The new
 * internal format has been designed by PLUMgrid:
 *
 *	Copyright (c) 2011 - 2014 PLUMgrid, http://plumgrid.com
 *
 * Authors:
 *
 *	Jay Schulist <jschlst@samba.org>
 *	Alexei Starovoitov <ast@plumgrid.com>
 *	Daniel Borkmann <dborkman@redhat.com>
 *
 * Andi Kleen - Fix a few bad bugs and races.
 * Kris Katterjohn - Added many additional checks in bpf_check_classic()
 */

#include <uapi/linux/btf.h>
#include <linux/filter.h>
#include <linux/skbuff.h>
#include <linux/vmalloc.h>
#include <linux/prandom.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/objtool.h>
#include <linux/overflow.h>
#include <linux/rbtree_latch.h>
#include <linux/kallsyms.h>
#include <linux/rcupdate.h>
#include <linux/perf_event.h>
#include <linux/extable.h>
#include <linux/log2.h>
#include <linux/bpf_verifier.h>
#include <linux/nodemask.h>
#include <linux/nospec.h>
#include <linux/bpf_mem_alloc.h>
#include <linux/memcontrol.h>
#include <linux/execmem.h>

#include <asm/barrier.h>
#include <linux/unaligned.h>

/* Registers */
#define BPF_R0	regs[BPF_REG_0]
#define BPF_R1	regs[BPF_REG_1]
#define BPF_R2	regs[BPF_REG_2]
#define BPF_R3	regs[BPF_REG_3]
#define BPF_R4	regs[BPF_REG_4]
#define BPF_R5	regs[BPF_REG_5]
#define BPF_R6	regs[BPF_REG_6]
#define BPF_R7	regs[BPF_REG_7]
#define BPF_R8	regs[BPF_REG_8]
#define BPF_R9	regs[BPF_REG_9]
#define BPF_R10	regs[BPF_REG_10]

/* Named registers */
#define DST	regs[insn->dst_reg]
#define SRC	regs[insn->src_reg]
#define FP	regs[BPF_REG_FP]
#define AX	regs[BPF_REG_AX]
#define ARG1	regs[BPF_REG_ARG1]
#define CTX	regs[BPF_REG_CTX]
#define OFF	insn->off
#define IMM	insn->imm

struct bpf_mem_alloc bpf_global_ma;
bool bpf_global_ma_set;

/* No hurry in this branch
 *
 * Exported for the bpf jit load helper.
 */
void *bpf_internal_load_pointer_neg_helper(const struct sk_buff *skb, int k, unsigned int size)
{
	u8 *ptr = NULL;

	if (k >= SKF_NET_OFF) {
		ptr = skb_network_header(skb) + k - SKF_NET_OFF;
	} else if (k >= SKF_LL_OFF) {
		if (unlikely(!skb_mac_header_was_set(skb)))
			return NULL;
		ptr = skb_mac_header(skb) + k - SKF_LL_OFF;
	}
	if (ptr >= skb->head && ptr + size <= skb_tail_pointer(skb))
		return ptr;

	return NULL;
}

/* tell bpf programs that include vmlinux.h kernel's PAGE_SIZE */
enum page_size_enum {
	__PAGE_SIZE = PAGE_SIZE
};

struct bpf_prog *bpf_prog_alloc_no_stats(unsigned int size, gfp_t gfp_extra_flags)
{
	gfp_t gfp_flags = bpf_memcg_flags(GFP_KERNEL | __GFP_ZERO | gfp_extra_flags);
	struct bpf_prog_aux *aux;
	struct bpf_prog *fp;

	size = round_up(size, __PAGE_SIZE);
	fp = __vmalloc(size, gfp_flags);
	if (fp == NULL)
		return NULL;

	aux = kzalloc(sizeof(*aux), bpf_memcg_flags(GFP_KERNEL | gfp_extra_flags));
	if (aux == NULL) {
		vfree(fp);
		return NULL;
	}
	fp->active = alloc_percpu_gfp(int, bpf_memcg_flags(GFP_KERNEL | gfp_extra_flags));
	if (!fp->active) {
		vfree(fp);
		kfree(aux);
		return NULL;
	}

	fp->pages = size / PAGE_SIZE;
	fp->aux = aux;
	fp->aux->prog = fp;
	fp->jit_requested = ebpf_jit_enabled();
	fp->blinding_requested = bpf_jit_blinding_enabled(fp);
#ifdef CONFIG_CGROUP_BPF
	aux->cgroup_atype = CGROUP_BPF_ATTACH_TYPE_INVALID;
#endif

	INIT_LIST_HEAD_RCU(&fp->aux->ksym.lnode);
#ifdef CONFIG_FINEIBT
	INIT_LIST_HEAD_RCU(&fp->aux->ksym_prefix.lnode);
#endif
	mutex_init(&fp->aux->used_maps_mutex);
	mutex_init(&fp->aux->ext_mutex);
	mutex_init(&fp->aux->dst_mutex);

	return fp;
}

struct bpf_prog *bpf_prog_alloc(unsigned int size, gfp_t gfp_extra_flags)
{
	gfp_t gfp_flags = bpf_memcg_flags(GFP_KERNEL | __GFP_ZERO | gfp_extra_flags);
	struct bpf_prog *prog;
	int cpu;

	prog = bpf_prog_alloc_no_stats(size, gfp_extra_flags);
	if (!prog)
		return NULL;

	prog->stats = alloc_percpu_gfp(struct bpf_prog_stats, gfp_flags);
	if (!prog->stats) {
		free_percpu(prog->active);
		kfree(prog->aux);
		vfree(prog);
		return NULL;
	}

	for_each_possible_cpu(cpu) {
		struct bpf_prog_stats *pstats;

		pstats = per_cpu_ptr(prog->stats, cpu);
		u64_stats_init(&pstats->syncp);
	}
	return prog;
}
EXPORT_SYMBOL_GPL(bpf_prog_alloc);

int bpf_prog_alloc_jited_linfo(struct bpf_prog *prog)
{
	if (!prog->aux->nr_linfo || !prog->jit_requested)
		return 0;

	prog->aux->jited_linfo = kvcalloc(prog->aux->nr_linfo,
					  sizeof(*prog->aux->jited_linfo),
					  bpf_memcg_flags(GFP_KERNEL | __GFP_NOWARN));
	if (!prog->aux->jited_linfo)
		return -ENOMEM;

	return 0;
}

void bpf_prog_jit_attempt_done(struct bpf_prog *prog)
{
	if (prog->aux->jited_linfo &&
	    (!prog->jited || !prog->aux->jited_linfo[0])) {
		kvfree(prog->aux->jited_linfo);
		prog->aux->jited_linfo = NULL;
	}

	kfree(prog->aux->kfunc_tab);
	prog->aux->kfunc_tab = NULL;
}

/* The jit engine is responsible to provide an array
 * for insn_off to the jited_off mapping (insn_to_jit_off).
 *
 * The idx to this array is the insn_off.  Hence, the insn_off
 * here is relative to the prog itself instead of the main prog.
 * This array has one entry for each xlated bpf insn.
 *
 * jited_off is the byte off to the end of the jited insn.
 *
 * Hence, with
 * insn_start:
 *      The first bpf insn off of the prog.  The insn off
 *      here is relative to the main prog.
 *      e.g. if prog is a subprog, insn_start > 0
 * linfo_idx:
 *      The prog's idx to prog->aux->linfo and jited_linfo
 *
 * jited_linfo[linfo_idx] = prog->bpf_func
 *
 * For i > linfo_idx,
 *
 * jited_linfo[i] = prog->bpf_func +
 *	insn_to_jit_off[linfo[i].insn_off - insn_start - 1]
 */
void bpf_prog_fill_jited_linfo(struct bpf_prog *prog,
			       const u32 *insn_to_jit_off)
{
	u32 linfo_idx, insn_start, insn_end, nr_linfo, i;
	const struct bpf_line_info *linfo;
	void **jited_linfo;

	if (!prog->aux->jited_linfo || prog->aux->func_idx > prog->aux->func_cnt)
		/* Userspace did not provide linfo */
		return;

	linfo_idx = prog->aux->linfo_idx;
	linfo = &prog->aux->linfo[linfo_idx];
	insn_start = linfo[0].insn_off;
	insn_end = insn_start + prog->len;

	jited_linfo = &prog->aux->jited_linfo[linfo_idx];
	jited_linfo[0] = prog->bpf_func;

	nr_linfo = prog->aux->nr_linfo - linfo_idx;

	for (i = 1; i < nr_linfo && linfo[i].insn_off < insn_end; i++)
		/* The verifier ensures that linfo[i].insn_off is
		 * strictly increasing
		 */
		jited_linfo[i] = prog->bpf_func +
			insn_to_jit_off[linfo[i].insn_off - insn_start - 1];
}

struct bpf_prog *bpf_prog_realloc(struct bpf_prog *fp_old, unsigned int size,
				  gfp_t gfp_extra_flags)
{
	gfp_t gfp_flags = bpf_memcg_flags(GFP_KERNEL | __GFP_ZERO | gfp_extra_flags);
	struct bpf_prog *fp;
	u32 pages;

	size = round_up(size, PAGE_SIZE);
	pages = size / PAGE_SIZE;
	if (pages <= fp_old->pages)
		return fp_old;

	fp = __vmalloc(size, gfp_flags);
	if (fp) {
		memcpy(fp, fp_old, fp_old->pages * PAGE_SIZE);
		fp->pages = pages;
		fp->aux->prog = fp;

		/* We keep fp->aux from fp_old around in the new
		 * reallocated structure.
		 */
		fp_old->aux = NULL;
		fp_old->stats = NULL;
		fp_old->active = NULL;
		__bpf_prog_free(fp_old);
	}

	return fp;
}

void __bpf_prog_free(struct bpf_prog *fp)
{
	if (fp->aux) {
		mutex_destroy(&fp->aux->used_maps_mutex);
		mutex_destroy(&fp->aux->dst_mutex);
		kfree(fp->aux->poke_tab);
		kfree(fp->aux);
	}
	free_percpu(fp->stats);
	free_percpu(fp->active);
	vfree(fp);
}

int bpf_prog_calc_tag(struct bpf_prog *fp)
{
	const u32 bits_offset = SHA1_BLOCK_SIZE - sizeof(__be64);
	u32 raw_size = bpf_prog_tag_scratch_size(fp);
	u32 digest[SHA1_DIGEST_WORDS];
	u32 ws[SHA1_WORKSPACE_WORDS];
	u32 i, bsize, psize, blocks;
	struct bpf_insn *dst;
	bool was_ld_map;
	u8 *raw, *todo;
	__be32 *result;
	__be64 *bits;

	raw = vmalloc(raw_size);
	if (!raw)
		return -ENOMEM;

	sha1_init_raw(digest);
	memset(ws, 0, sizeof(ws));

	/* We need to take out the map fd for the digest calculation
	 * since they are unstable from user space side.
	 */
	dst = (void *)raw;
	for (i = 0, was_ld_map = false; i < fp->len; i++) {
		dst[i] = fp->insnsi[i];
		if (!was_ld_map &&
		    dst[i].code == (BPF_LD | BPF_IMM | BPF_DW) &&
		    (dst[i].src_reg == BPF_PSEUDO_MAP_FD ||
		     dst[i].src_reg == BPF_PSEUDO_MAP_VALUE)) {
			was_ld_map = true;
			dst[i].imm = 0;
		} else if (was_ld_map &&
			   dst[i].code == 0 &&
			   dst[i].dst_reg == 0 &&
			   dst[i].src_reg == 0 &&
			   dst[i].off == 0) {
			was_ld_map = false;
			dst[i].imm = 0;
		} else {
			was_ld_map = false;
		}
	}

	psize = bpf_prog_insn_size(fp);
	memset(&raw[psize], 0, raw_size - psize);
	raw[psize++] = 0x80;

	bsize  = round_up(psize, SHA1_BLOCK_SIZE);
	blocks = bsize / SHA1_BLOCK_SIZE;
	todo   = raw;
	if (bsize - psize >= sizeof(__be64)) {
		bits = (__be64 *)(todo + bsize - sizeof(__be64));
	} else {
		bits = (__be64 *)(todo + bsize + bits_offset);
		blocks++;
	}
	*bits = cpu_to_be64((psize - 1) << 3);

	while (blocks--) {
		sha1_transform(digest, todo, ws);
		todo += SHA1_BLOCK_SIZE;
	}

	result = (__force __be32 *)digest;
	for (i = 0; i < SHA1_DIGEST_WORDS; i++)
		result[i] = cpu_to_be32(digest[i]);
	memcpy(fp->tag, result, sizeof(fp->tag));

	vfree(raw);
	return 0;
}

static int bpf_adj_delta_to_imm(struct bpf_insn *insn, u32 pos, s32 end_old,
				s32 end_new, s32 curr, const bool probe_pass)
{
	const s64 imm_min = S32_MIN, imm_max = S32_MAX;
	s32 delta = end_new - end_old;
	s64 imm = insn->imm;

	if (curr < pos && curr + imm + 1 >= end_old)
		imm += delta;
	else if (curr >= end_new && curr + imm + 1 < end_new)
		imm -= delta;
	if (imm < imm_min || imm > imm_max)
		return -ERANGE;
	if (!probe_pass)
		insn->imm = imm;
	return 0;
}

static int bpf_adj_delta_to_off(struct bpf_insn *insn, u32 pos, s32 end_old,
				s32 end_new, s32 curr, const bool probe_pass)
{
	s64 off_min, off_max, off;
	s32 delta = end_new - end_old;

	if (insn->code == (BPF_JMP32 | BPF_JA)) {
		off = insn->imm;
		off_min = S32_MIN;
		off_max = S32_MAX;
	} else {
		off = insn->off;
		off_min = S16_MIN;
		off_max = S16_MAX;
	}

	if (curr < pos && curr + off + 1 >= end_old)
		off += delta;
	else if (curr >= end_new && curr + off + 1 < end_new)
		off -= delta;
	if (off < off_min || off > off_max)
		return -ERANGE;
	if (!probe_pass) {
		if (insn->code == (BPF_JMP32 | BPF_JA))
			insn->imm = off;
		else
			insn->off = off;
	}
	return 0;
}

static int bpf_adj_branches(struct bpf_prog *prog, u32 pos, s32 end_old,
			    s32 end_new, const bool probe_pass)
{
	u32 i, insn_cnt = prog->len + (probe_pass ? end_new - end_old : 0);
	struct bpf_insn *insn = prog->insnsi;
	int ret = 0;

	for (i = 0; i < insn_cnt; i++, insn++) {
		u8 code;

		/* In the probing pass we still operate on the original,
		 * unpatched image in order to check overflows before we
		 * do any other adjustments. Therefore skip the patchlet.
		 */
		if (probe_pass && i == pos) {
			i = end_new;
			insn = prog->insnsi + end_old;
		}
		if (bpf_pseudo_func(insn)) {
			ret = bpf_adj_delta_to_imm(insn, pos, end_old,
						   end_new, i, probe_pass);
			if (ret)
				return ret;
			continue;
		}
		code = insn->code;
		if ((BPF_CLASS(code) != BPF_JMP &&
		     BPF_CLASS(code) != BPF_JMP32) ||
		    BPF_OP(code) == BPF_EXIT)
			continue;
		/* Adjust offset of jmps if we cross patch boundaries. */
		if (BPF_OP(code) == BPF_CALL) {
			if (insn->src_reg != BPF_PSEUDO_CALL)
				continue;
			ret = bpf_adj_delta_to_imm(insn, pos, end_old,
						   end_new, i, probe_pass);
		} else {
			ret = bpf_adj_delta_to_off(insn, pos, end_old,
						   end_new, i, probe_pass);
		}
		if (ret)
			break;
	}

	return ret;
}

static void bpf_adj_linfo(struct bpf_prog *prog, u32 off, u32 delta)
{
	struct bpf_line_info *linfo;
	u32 i, nr_linfo;

	nr_linfo = prog->aux->nr_linfo;
	if (!nr_linfo || !delta)
		return;

	linfo = prog->aux->linfo;

	for (i = 0; i < nr_linfo; i++)
		if (off < linfo[i].insn_off)
			break;

	/* Push all off < linfo[i].insn_off by delta */
	for (; i < nr_linfo; i++)
		linfo[i].insn_off += delta;
}

struct bpf_prog *bpf_patch_insn_single(struct bpf_prog *prog, u32 off,
				       const struct bpf_insn *patch, u32 len)
{
	u32 insn_adj_cnt, insn_rest, insn_delta = len - 1;
	const u32 cnt_max = S16_MAX;
	struct bpf_prog *prog_adj;
	int err;

	/* Since our patchlet doesn't expand the image, we're done. */
	if (insn_delta == 0) {
		memcpy(prog->insnsi + off, patch, sizeof(*patch));
		return prog;
	}

	insn_adj_cnt = prog->len + insn_delta;

	/* Reject anything that would potentially let the insn->off
	 * target overflow when we have excessive program expansions.
	 * We need to probe here before we do any reallocation where
	 * we afterwards may not fail anymore.
	 */
	if (insn_adj_cnt > cnt_max &&
	    (err = bpf_adj_branches(prog, off, off + 1, off + len, true)))
		return ERR_PTR(err);

	/* Several new instructions need to be inserted. Make room
	 * for them. Likely, there's no need for a new allocation as
	 * last page could have large enough tailroom.
	 */
	prog_adj = bpf_prog_realloc(prog, bpf_prog_size(insn_adj_cnt),
				    GFP_USER);
	if (!prog_adj)
		return ERR_PTR(-ENOMEM);

	prog_adj->len = insn_adj_cnt;

	/* Patching happens in 3 steps:
	 *
	 * 1) Move over tail of insnsi from next instruction onwards,
	 *    so we can patch the single target insn with one or more
	 *    new ones (patching is always from 1 to n insns, n > 0).
	 * 2) Inject new instructions at the target location.
	 * 3) Adjust branch offsets if necessary.
	 */
	insn_rest = insn_adj_cnt - off - len;

	memmove(prog_adj->insnsi + off + len, prog_adj->insnsi + off + 1,
		sizeof(*patch) * insn_rest);
	memcpy(prog_adj->insnsi + off, patch, sizeof(*patch) * len);

	/* We are guaranteed to not fail at this point, otherwise
	 * the ship has sailed to reverse to the original state. An
	 * overflow cannot happen at this point.
	 */
	BUG_ON(bpf_adj_branches(prog_adj, off, off + 1, off + len, false));

	bpf_adj_linfo(prog_adj, off, insn_delta);

	return prog_adj;
}

int bpf_remove_insns(struct bpf_prog *prog, u32 off, u32 cnt)
{
	int err;

	/* Branch offsets can't overflow when program is shrinking, no need
	 * to call bpf_adj_branches(..., true) here
	 */
	memmove(prog->insnsi + off, prog->insnsi + off + cnt,
		sizeof(struct bpf_insn) * (prog->len - off - cnt));
	prog->len -= cnt;

	err = bpf_adj_branches(prog, off, off + cnt, off, false);
	WARN_ON_ONCE(err);
	return err;
}

static void bpf_prog_kallsyms_del_subprogs(struct bpf_prog *fp)
{
	int i;

	for (i = 0; i < fp->aux->real_func_cnt; i++)
		bpf_prog_kallsyms_del(fp->aux->func[i]);
}

void bpf_prog_kallsyms_del_all(struct bpf_prog *fp)
{
	bpf_prog_kallsyms_del_subprogs(fp);
	bpf_prog_kallsyms_del(fp);
}

#ifdef CONFIG_BPF_JIT
/* All BPF JIT sysctl knobs here. */
int bpf_jit_enable   __read_mostly = IS_BUILTIN(CONFIG_BPF_JIT_DEFAULT_ON);
int bpf_jit_kallsyms __read_mostly = IS_BUILTIN(CONFIG_BPF_JIT_DEFAULT_ON);
int bpf_jit_harden   __read_mostly;
long bpf_jit_limit   __read_mostly;
long bpf_jit_limit_max __read_mostly;

static void
bpf_prog_ksym_set_addr(struct bpf_prog *prog)
{
	WARN_ON_ONCE(!bpf_prog_ebpf_jited(prog));

	prog->aux->ksym.start = (unsigned long) prog->bpf_func;
	prog->aux->ksym.end   = prog->aux->ksym.start + prog->jited_len;
}

static void
bpf_prog_ksym_set_name(struct bpf_prog *prog)
{
	char *sym = prog->aux->ksym.name;
	const char *end = sym + KSYM_NAME_LEN;
	const struct btf_type *type;
	const char *func_name;

	BUILD_BUG_ON(sizeof("bpf_prog_") +
		     sizeof(prog->tag) * 2 +
		     /* name has been null terminated.
		      * We should need +1 for the '_' preceding
		      * the name.  However, the null character
		      * is double counted between the name and the
		      * sizeof("bpf_prog_") above, so we omit
		      * the +1 here.
		      */
		     sizeof(prog->aux->name) > KSYM_NAME_LEN);

	sym += snprintf(sym, KSYM_NAME_LEN, "bpf_prog_");
	sym  = bin2hex(sym, prog->tag, sizeof(prog->tag));

	/* prog->aux->name will be ignored if full btf name is available */
	if (prog->aux->func_info_cnt && prog->aux->func_idx < prog->aux->func_info_cnt) {
		type = btf_type_by_id(prog->aux->btf,
				      prog->aux->func_info[prog->aux->func_idx].type_id);
		func_name = btf_name_by_offset(prog->aux->btf, type->name_off);
		snprintf(sym, (size_t)(end - sym), "_%s", func_name);
		return;
	}

	if (prog->aux->name[0])
		snprintf(sym, (size_t)(end - sym), "_%s", prog->aux->name);
	else
		*sym = 0;
}

static unsigned long bpf_get_ksym_start(struct latch_tree_node *n)
{
	return container_of(n, struct bpf_ksym, tnode)->start;
}

static __always_inline bool bpf_tree_less(struct latch_tree_node *a,
					  struct latch_tree_node *b)
{
	return bpf_get_ksym_start(a) < bpf_get_ksym_start(b);
}

static __always_inline int bpf_tree_comp(void *key, struct latch_tree_node *n)
{
	unsigned long val = (unsigned long)key;
	const struct bpf_ksym *ksym;

	ksym = container_of(n, struct bpf_ksym, tnode);

	if (val < ksym->start)
		return -1;
	/* Ensure that we detect return addresses as part of the program, when
	 * the final instruction is a call for a program part of the stack
	 * trace. Therefore, do val > ksym->end instead of val >= ksym->end.
	 */
	if (val > ksym->end)
		return  1;

	return 0;
}

static const struct latch_tree_ops bpf_tree_ops = {
	.less	= bpf_tree_less,
	.comp	= bpf_tree_comp,
};

static DEFINE_SPINLOCK(bpf_lock);
static LIST_HEAD(bpf_kallsyms);
static struct latch_tree_root bpf_tree __cacheline_aligned;

void bpf_ksym_add(struct bpf_ksym *ksym)
{
	spin_lock_bh(&bpf_lock);
	WARN_ON_ONCE(!list_empty(&ksym->lnode));
	list_add_tail_rcu(&ksym->lnode, &bpf_kallsyms);
	latch_tree_insert(&ksym->tnode, &bpf_tree, &bpf_tree_ops);
	spin_unlock_bh(&bpf_lock);
}

static void __bpf_ksym_del(struct bpf_ksym *ksym)
{
	if (list_empty(&ksym->lnode))
		return;

	latch_tree_erase(&ksym->tnode, &bpf_tree, &bpf_tree_ops);
	list_del_rcu(&ksym->lnode);
}

void bpf_ksym_del(struct bpf_ksym *ksym)
{
	spin_lock_bh(&bpf_lock);
	__bpf_ksym_del(ksym);
	spin_unlock_bh(&bpf_lock);
}

static bool bpf_prog_kallsyms_candidate(const struct bpf_prog *fp)
{
	return fp->jited && !bpf_prog_was_classic(fp);
}

void bpf_prog_kallsyms_add(struct bpf_prog *fp)
{
	if (!bpf_prog_kallsyms_candidate(fp) ||
	    !bpf_token_capable(fp->aux->token, CAP_BPF))
		return;

	bpf_prog_ksym_set_addr(fp);
	bpf_prog_ksym_set_name(fp);
	fp->aux->ksym.prog = true;

	bpf_ksym_add(&fp->aux->ksym);

#ifdef CONFIG_FINEIBT
	/*
	 * When FineIBT, code in the __cfi_foo() symbols can get executed
	 * and hence unwinder needs help.
	 */
	if (cfi_mode != CFI_FINEIBT)
		return;

	snprintf(fp->aux->ksym_prefix.name, KSYM_NAME_LEN,
		 "__cfi_%s", fp->aux->ksym.name);

	fp->aux->ksym_prefix.start = (unsigned long) fp->bpf_func - 16;
	fp->aux->ksym_prefix.end   = (unsigned long) fp->bpf_func;

	bpf_ksym_add(&fp->aux->ksym_prefix);
#endif
}

void bpf_prog_kallsyms_del(struct bpf_prog *fp)
{
	if (!bpf_prog_kallsyms_candidate(fp))
		return;

	bpf_ksym_del(&fp->aux->ksym);
#ifdef CONFIG_FINEIBT
	if (cfi_mode != CFI_FINEIBT)
		return;
	bpf_ksym_del(&fp->aux->ksym_prefix);
#endif
}

static struct bpf_ksym *bpf_ksym_find(unsigned long addr)
{
	struct latch_tree_node *n;

	n = latch_tree_find((void *)addr, &bpf_tree, &bpf_tree_ops);
	return n ? container_of(n, struct bpf_ksym, tnode) : NULL;
}

int __bpf_address_lookup(unsigned long addr, unsigned long *size,
				 unsigned long *off, char *sym)
{
	struct bpf_ksym *ksym;
	int ret = 0;

	rcu_read_lock();
	ksym = bpf_ksym_find(addr);
	if (ksym) {
		unsigned long symbol_start = ksym->start;
		unsigned long symbol_end = ksym->end;

		ret = strscpy(sym, ksym->name, KSYM_NAME_LEN);

		if (size)
			*size = symbol_end - symbol_start;
		if (off)
			*off  = addr - symbol_start;
	}
	rcu_read_unlock();

	return ret;
}

bool is_bpf_text_address(unsigned long addr)
{
	bool ret;

	rcu_read_lock();
	ret = bpf_ksym_find(addr) != NULL;
	rcu_read_unlock();

	return ret;
}

struct bpf_prog *bpf_prog_ksym_find(unsigned long addr)
{
	struct bpf_ksym *ksym = bpf_ksym_find(addr);

	return ksym && ksym->prog ?
	       container_of(ksym, struct bpf_prog_aux, ksym)->prog :
	       NULL;
}

const struct exception_table_entry *search_bpf_extables(unsigned long addr)
{
	const struct exception_table_entry *e = NULL;
	struct bpf_prog *prog;

	rcu_read_lock();
	prog = bpf_prog_ksym_find(addr);
	if (!prog)
		goto out;
	if (!prog->aux->num_exentries)
		goto out;

	e = search_extable(prog->aux->extable, prog->aux->num_exentries, addr);
out:
	rcu_read_unlock();
	return e;
}

int bpf_get_kallsym(unsigned int symnum, unsigned long *value, char *type,
		    char *sym)
{
	struct bpf_ksym *ksym;
	unsigned int it = 0;
	int ret = -ERANGE;

	if (!bpf_jit_kallsyms_enabled())
		return ret;

	rcu_read_lock();
	list_for_each_entry_rcu(ksym, &bpf_kallsyms, lnode) {
		if (it++ != symnum)
			continue;

		strscpy(sym, ksym->name, KSYM_NAME_LEN);

		*value = ksym->start;
		*type  = BPF_SYM_ELF_TYPE;

		ret = 0;
		break;
	}
	rcu_read_unlock();

	return ret;
}

int bpf_jit_add_poke_descriptor(struct bpf_prog *prog,
				struct bpf_jit_poke_descriptor *poke)
{
	struct bpf_jit_poke_descriptor *tab = prog->aux->poke_tab;
	static const u32 poke_tab_max = 1024;
	u32 slot = prog->aux->size_poke_tab;
	u32 size = slot + 1;

	if (size > poke_tab_max)
		return -ENOSPC;
	if (poke->tailcall_target || poke->tailcall_target_stable ||
	    poke->tailcall_bypass || poke->adj_off || poke->bypass_addr)
		return -EINVAL;

	switch (poke->reason) {
	case BPF_POKE_REASON_TAIL_CALL:
		if (!poke->tail_call.map)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	tab = krealloc_array(tab, size, sizeof(*poke), GFP_KERNEL);
	if (!tab)
		return -ENOMEM;

	memcpy(&tab[slot], poke, sizeof(*poke));
	prog->aux->size_poke_tab = size;
	prog->aux->poke_tab = tab;

	return slot;
}

/*
 * BPF program pack allocator.
 *
 * Most BPF programs are pretty small. Allocating a hole page for each
 * program is sometime a waste. Many small bpf program also adds pressure
 * to instruction TLB. To solve this issue, we introduce a BPF program pack
 * allocator. The prog_pack allocator uses HPAGE_PMD_SIZE page (2MB on x86)
 * to host BPF programs.
 */
#define BPF_PROG_CHUNK_SHIFT	6
#define BPF_PROG_CHUNK_SIZE	(1 << BPF_PROG_CHUNK_SHIFT)
#define BPF_PROG_CHUNK_MASK	(~(BPF_PROG_CHUNK_SIZE - 1))

struct bpf_prog_pack {
	struct list_head list;
	void *ptr;
	unsigned long bitmap[];
};

void bpf_jit_fill_hole_with_zero(void *area, unsigned int size)
{
	memset(area, 0, size);
}

#define BPF_PROG_SIZE_TO_NBITS(size)	(round_up(size, BPF_PROG_CHUNK_SIZE) / BPF_PROG_CHUNK_SIZE)

static DEFINE_MUTEX(pack_mutex);
static LIST_HEAD(pack_list);

/* PMD_SIZE is not available in some special config, e.g. ARCH=arm with
 * CONFIG_MMU=n. Use PAGE_SIZE in these cases.
 */
#ifdef PMD_SIZE
/* PMD_SIZE is really big for some archs. It doesn't make sense to
 * reserve too much memory in one allocation. Hardcode BPF_PROG_PACK_SIZE to
 * 2MiB * num_possible_nodes(). On most architectures PMD_SIZE will be
 * greater than or equal to 2MB.
 */
#define BPF_PROG_PACK_SIZE (SZ_2M * num_possible_nodes())
#else
#define BPF_PROG_PACK_SIZE PAGE_SIZE
#endif

#define BPF_PROG_CHUNK_COUNT (BPF_PROG_PACK_SIZE / BPF_PROG_CHUNK_SIZE)

static struct bpf_prog_pack *alloc_new_pack(bpf_jit_fill_hole_t bpf_fill_ill_insns)
{
	struct bpf_prog_pack *pack;
	int err;

	pack = kzalloc(struct_size(pack, bitmap, BITS_TO_LONGS(BPF_PROG_CHUNK_COUNT)),
		       GFP_KERNEL);
	if (!pack)
		return NULL;
	pack->ptr = bpf_jit_alloc_exec(BPF_PROG_PACK_SIZE);
	if (!pack->ptr)
		goto out;
	bpf_fill_ill_insns(pack->ptr, BPF_PROG_PACK_SIZE);
	bitmap_zero(pack->bitmap, BPF_PROG_PACK_SIZE / BPF_PROG_CHUNK_SIZE);

	set_vm_flush_reset_perms(pack->ptr);
	err = set_memory_rox((unsigned long)pack->ptr,
			     BPF_PROG_PACK_SIZE / PAGE_SIZE);
	if (err)
		goto out;
	list_add_tail(&pack->list, &pack_list);
	return pack;

out:
	bpf_jit_free_exec(pack->ptr);
	kfree(pack);
	return NULL;
}

void *bpf_prog_pack_alloc(u32 size, bpf_jit_fill_hole_t bpf_fill_ill_insns)
{
	unsigned int nbits = BPF_PROG_SIZE_TO_NBITS(size);
	struct bpf_prog_pack *pack;
	unsigned long pos;
	void *ptr = NULL;

	mutex_lock(&pack_mutex);
	if (size > BPF_PROG_PACK_SIZE) {
		size = round_up(size, PAGE_SIZE);
		ptr = bpf_jit_alloc_exec(size);
		if (ptr) {
			int err;

			bpf_fill_ill_insns(ptr, size);
			set_vm_flush_reset_perms(ptr);
			err = set_memory_rox((unsigned long)ptr,
					     size / PAGE_SIZE);
			if (err) {
				bpf_jit_free_exec(ptr);
				ptr = NULL;
			}
		}
		goto out;
	}
	list_for_each_entry(pack, &pack_list, list) {
		pos = bitmap_find_next_zero_area(pack->bitmap, BPF_PROG_CHUNK_COUNT, 0,
						 nbits, 0);
		if (pos < BPF_PROG_CHUNK_COUNT)
			goto found_free_area;
	}

	pack = alloc_new_pack(bpf_fill_ill_insns);
	if (!pack)
		goto out;

	pos = 0;

found_free_area:
	bitmap_set(pack->bitmap, pos, nbits);
	ptr = (void *)(pack->ptr) + (pos << BPF_PROG_CHUNK_SHIFT);

out:
	mutex_unlock(&pack_mutex);
	return ptr;
}

void bpf_prog_pack_free(void *ptr, u32 size)
{
	struct bpf_prog_pack *pack = NULL, *tmp;
	unsigned int nbits;
	unsigned long pos;

	mutex_lock(&pack_mutex);
	if (size > BPF_PROG_PACK_SIZE) {
		bpf_jit_free_exec(ptr);
		goto out;
	}

	list_for_each_entry(tmp, &pack_list, list) {
		if (ptr >= tmp->ptr && (tmp->ptr + BPF_PROG_PACK_SIZE) > ptr) {
			pack = tmp;
			break;
		}
	}

	if (WARN_ONCE(!pack, "bpf_prog_pack bug\n"))
		goto out;

	nbits = BPF_PROG_SIZE_TO_NBITS(size);
	pos = ((unsigned long)ptr - (unsigned long)pack->ptr) >> BPF_PROG_CHUNK_SHIFT;

	WARN_ONCE(bpf_arch_text_invalidate(ptr, size),
		  "bpf_prog_pack bug: missing bpf_arch_text_invalidate?\n");

	bitmap_clear(pack->bitmap, pos, nbits);
	if (bitmap_find_next_zero_area(pack->bitmap, BPF_PROG_CHUNK_COUNT, 0,
				       BPF_PROG_CHUNK_COUNT, 0) == 0) {
		list_del(&pack->list);
		bpf_jit_free_exec(pack->ptr);
		kfree(pack);
	}
out:
	mutex_unlock(&pack_mutex);
}

static atomic_long_t bpf_jit_current;

/* Can be overridden by an arch's JIT compiler if it has a custom,
 * dedicated BPF backend memory area, or if neither of the two
 * below apply.
 */
u64 __weak bpf_jit_alloc_exec_limit(void)
{
#if defined(MODULES_VADDR)
	return MODULES_END - MODULES_VADDR;
#else
	return VMALLOC_END - VMALLOC_START;
#endif
}

static int __init bpf_jit_charge_init(void)
{
	/* Only used as heuristic here to derive limit. */
	bpf_jit_limit_max = bpf_jit_alloc_exec_limit();
	bpf_jit_limit = min_t(u64, round_up(bpf_jit_limit_max >> 1,
					    PAGE_SIZE), LONG_MAX);
	return 0;
}
pure_initcall(bpf_jit_charge_init);

int bpf_jit_charge_modmem(u32 size)
{
	if (atomic_long_add_return(size, &bpf_jit_current) > READ_ONCE(bpf_jit_limit)) {
		if (!bpf_capable()) {
			atomic_long_sub(size, &bpf_jit_current);
			return -EPERM;
		}
	}

	return 0;
}

void bpf_jit_uncharge_modmem(u32 size)
{
	atomic_long_sub(size, &bpf_jit_current);
}

void *__weak bpf_jit_alloc_exec(unsigned long size)
{
	return execmem_alloc(EXECMEM_BPF, size);
}

void __weak bpf_jit_free_exec(void *addr)
{
	execmem_free(addr);
}

struct bpf_binary_header *
bpf_jit_binary_alloc(unsigned int proglen, u8 **image_ptr,
		     unsigned int alignment,
		     bpf_jit_fill_hole_t bpf_fill_ill_insns)
{
	struct bpf_binary_header *hdr;
	u32 size, hole, start;

	WARN_ON_ONCE(!is_power_of_2(alignment) ||
		     alignment > BPF_IMAGE_ALIGNMENT);

	/* Most of BPF filters are really small, but if some of them
	 * fill a page, allow at least 128 extra bytes to insert a
	 * random section of illegal instructions.
	 */
	size = round_up(proglen + sizeof(*hdr) + 128, PAGE_SIZE);

	if (bpf_jit_charge_modmem(size))
		return NULL;
	hdr = bpf_jit_alloc_exec(size);
	if (!hdr) {
		bpf_jit_uncharge_modmem(size);
		return NULL;
	}

	/* Fill space with illegal/arch-dep instructions. */
	bpf_fill_ill_insns(hdr, size);

	hdr->size = size;
	hole = min_t(unsigned int, size - (proglen + sizeof(*hdr)),
		     PAGE_SIZE - sizeof(*hdr));
	start = get_random_u32_below(hole) & ~(alignment - 1);

	/* Leave a random number of instructions before BPF code. */
	*image_ptr = &hdr->image[start];

	return hdr;
}

void bpf_jit_binary_free(struct bpf_binary_header *hdr)
{
	u32 size = hdr->size;

	bpf_jit_free_exec(hdr);
	bpf_jit_uncharge_modmem(size);
}

/* Allocate jit binary from bpf_prog_pack allocator.
 * Since the allocated memory is RO+X, the JIT engine cannot write directly
 * to the memory. To solve this problem, a RW buffer is also allocated at
 * as the same time. The JIT engine should calculate offsets based on the
 * RO memory address, but write JITed program to the RW buffer. Once the
 * JIT engine finishes, it calls bpf_jit_binary_pack_finalize, which copies
 * the JITed program to the RO memory.
 */
struct bpf_binary_header *
bpf_jit_binary_pack_alloc(unsigned int proglen, u8 **image_ptr,
			  unsigned int alignment,
			  struct bpf_binary_header **rw_header,
			  u8 **rw_image,
			  bpf_jit_fill_hole_t bpf_fill_ill_insns)
{
	struct bpf_binary_header *ro_header;
	u32 size, hole, start;

	WARN_ON_ONCE(!is_power_of_2(alignment) ||
		     alignment > BPF_IMAGE_ALIGNMENT);

	/* add 16 bytes for a random section of illegal instructions */
	size = round_up(proglen + sizeof(*ro_header) + 16, BPF_PROG_CHUNK_SIZE);

	if (bpf_jit_charge_modmem(size))
		return NULL;
	ro_header = bpf_prog_pack_alloc(size, bpf_fill_ill_insns);
	if (!ro_header) {
		bpf_jit_uncharge_modmem(size);
		return NULL;
	}

	*rw_header = kvmalloc(size, GFP_KERNEL);
	if (!*rw_header) {
		bpf_prog_pack_free(ro_header, size);
		bpf_jit_uncharge_modmem(size);
		return NULL;
	}

	/* Fill space with illegal/arch-dep instructions. */
	bpf_fill_ill_insns(*rw_header, size);
	(*rw_header)->size = size;

	hole = min_t(unsigned int, size - (proglen + sizeof(*ro_header)),
		     BPF_PROG_CHUNK_SIZE - sizeof(*ro_header));
	start = get_random_u32_below(hole) & ~(alignment - 1);

	*image_ptr = &ro_header->image[start];
	*rw_image = &(*rw_header)->image[start];

	return ro_header;
}

/* Copy JITed text from rw_header to its final location, the ro_header. */
int bpf_jit_binary_pack_finalize(struct bpf_binary_header *ro_header,
				 struct bpf_binary_header *rw_header)
{
	void *ptr;

	ptr = bpf_arch_text_copy(ro_header, rw_header, rw_header->size);

	kvfree(rw_header);

	if (IS_ERR(ptr)) {
		bpf_prog_pack_free(ro_header, ro_header->size);
		return PTR_ERR(ptr);
	}
	return 0;
}

/* bpf_jit_binary_pack_free is called in two different scenarios:
 *   1) when the program is freed after;
 *   2) when the JIT engine fails (before bpf_jit_binary_pack_finalize).
 * For case 2), we need to free both the RO memory and the RW buffer.
 *
 * bpf_jit_binary_pack_free requires proper ro_header->size. However,
 * bpf_jit_binary_pack_alloc does not set it. Therefore, ro_header->size
 * must be set with either bpf_jit_binary_pack_finalize (normal path) or
 * bpf_arch_text_copy (when jit fails).
 */
void bpf_jit_binary_pack_free(struct bpf_binary_header *ro_header,
			      struct bpf_binary_header *rw_header)
{
	u32 size = ro_header->size;

	bpf_prog_pack_free(ro_header, size);
	kvfree(rw_header);
	bpf_jit_uncharge_modmem(size);
}

struct bpf_binary_header *
bpf_jit_binary_pack_hdr(const struct bpf_prog *fp)
{
	unsigned long real_start = (unsigned long)fp->bpf_func;
	unsigned long addr;

	addr = real_start & BPF_PROG_CHUNK_MASK;
	return (void *)addr;
}

static inline struct bpf_binary_header *
bpf_jit_binary_hdr(const struct bpf_prog *fp)
{
	unsigned long real_start = (unsigned long)fp->bpf_func;
	unsigned long addr;

	addr = real_start & PAGE_MASK;
	return (void *)addr;
}

/* This symbol is only overridden by archs that have different
 * requirements than the usual eBPF JITs, f.e. when they only
 * implement cBPF JIT, do not set images read-only, etc.
 */
void __weak bpf_jit_free(struct bpf_prog *fp)
{
	if (fp->jited) {
		struct bpf_binary_header *hdr = bpf_jit_binary_hdr(fp);

		bpf_jit_binary_free(hdr);
		WARN_ON_ONCE(!bpf_prog_kallsyms_verify_off(fp));
	}

	bpf_prog_unlock_free(fp);
}

int bpf_jit_get_func_addr(const struct bpf_prog *prog,
			  const struct bpf_insn *insn, bool extra_pass,
			  u64 *func_addr, bool *func_addr_fixed)
{
	s16 off = insn->off;
	s32 imm = insn->imm;
	u8 *addr;
	int err;

	*func_addr_fixed = insn->src_reg != BPF_PSEUDO_CALL;
	if (!*func_addr_fixed) {
		/* Place-holder address till the last pass has collected
		 * all addresses for JITed subprograms in which case we
		 * can pick them up from prog->aux.
		 */
		if (!extra_pass)
			addr = NULL;
		else if (prog->aux->func &&
			 off >= 0 && off < prog->aux->real_func_cnt)
			addr = (u8 *)prog->aux->func[off]->bpf_func;
		else
			return -EINVAL;
	} else if (insn->src_reg == BPF_PSEUDO_KFUNC_CALL &&
		   bpf_jit_supports_far_kfunc_call()) {
		err = bpf_get_kfunc_addr(prog, insn->imm, insn->off, &addr);
		if (err)
			return err;
	} else {
		/* Address of a BPF helper call. Since part of the core
		 * kernel, it's always at a fixed location. __bpf_call_base
		 * and the helper with imm relative to it are both in core
		 * kernel.
		 */
		addr = (u8 *)__bpf_call_base + imm;
	}

	*func_addr = (unsigned long)addr;
	return 0;
}

static int bpf_jit_blind_insn(const struct bpf_insn *from,
			      const struct bpf_insn *aux,
			      struct bpf_insn *to_buff,
			      bool emit_zext)
{
	struct bpf_insn *to = to_buff;
	u32 imm_rnd = get_random_u32();
	s16 off;

	BUILD_BUG_ON(BPF_REG_AX  + 1 != MAX_BPF_JIT_REG);
	BUILD_BUG_ON(MAX_BPF_REG + 1 != MAX_BPF_JIT_REG);

	/* Constraints on AX register:
	 *
	 * AX register is inaccessible from user space. It is mapped in
	 * all JITs, and used here for constant blinding rewrites. It is
	 * typically "stateless" meaning its contents are only valid within
	 * the executed instruction, but not across several instructions.
	 * There are a few exceptions however which are further detailed
	 * below.
	 *
	 * Constant blinding is only used by JITs, not in the interpreter.
	 * The interpreter uses AX in some occasions as a local temporary
	 * register e.g. in DIV or MOD instructions.
	 *
	 * In restricted circumstances, the verifier can also use the AX
	 * register for rewrites as long as they do not interfere with
	 * the above cases!
	 */
	if (from->dst_reg == BPF_REG_AX || from->src_reg == BPF_REG_AX)
		goto out;

	if (from->imm == 0 &&
	    (from->code == (BPF_ALU   | BPF_MOV | BPF_K) ||
	     from->code == (BPF_ALU64 | BPF_MOV | BPF_K))) {
		*to++ = BPF_ALU64_REG(BPF_XOR, from->dst_reg, from->dst_reg);
		goto out;
	}

	switch (from->code) {
	case BPF_ALU | BPF_ADD | BPF_K:
	case BPF_ALU | BPF_SUB | BPF_K:
	case BPF_ALU | BPF_AND | BPF_K:
	case BPF_ALU | BPF_OR  | BPF_K:
	case BPF_ALU | BPF_XOR | BPF_K:
	case BPF_ALU | BPF_MUL | BPF_K:
	case BPF_ALU | BPF_MOV | BPF_K:
	case BPF_ALU | BPF_DIV | BPF_K:
	case BPF_ALU | BPF_MOD | BPF_K:
		*to++ = BPF_ALU32_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU32_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_ALU32_REG_OFF(from->code, from->dst_reg, BPF_REG_AX, from->off);
		break;

	case BPF_ALU64 | BPF_ADD | BPF_K:
	case BPF_ALU64 | BPF_SUB | BPF_K:
	case BPF_ALU64 | BPF_AND | BPF_K:
	case BPF_ALU64 | BPF_OR  | BPF_K:
	case BPF_ALU64 | BPF_XOR | BPF_K:
	case BPF_ALU64 | BPF_MUL | BPF_K:
	case BPF_ALU64 | BPF_MOV | BPF_K:
	case BPF_ALU64 | BPF_DIV | BPF_K:
	case BPF_ALU64 | BPF_MOD | BPF_K:
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_ALU64_REG_OFF(from->code, from->dst_reg, BPF_REG_AX, from->off);
		break;

	case BPF_JMP | BPF_JEQ  | BPF_K:
	case BPF_JMP | BPF_JNE  | BPF_K:
	case BPF_JMP | BPF_JGT  | BPF_K:
	case BPF_JMP | BPF_JLT  | BPF_K:
	case BPF_JMP | BPF_JGE  | BPF_K:
	case BPF_JMP | BPF_JLE  | BPF_K:
	case BPF_JMP | BPF_JSGT | BPF_K:
	case BPF_JMP | BPF_JSLT | BPF_K:
	case BPF_JMP | BPF_JSGE | BPF_K:
	case BPF_JMP | BPF_JSLE | BPF_K:
	case BPF_JMP | BPF_JSET | BPF_K:
		/* Accommodate for extra offset in case of a backjump. */
		off = from->off;
		if (off < 0)
			off -= 2;
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_JMP_REG(from->code, from->dst_reg, BPF_REG_AX, off);
		break;

	case BPF_JMP32 | BPF_JEQ  | BPF_K:
	case BPF_JMP32 | BPF_JNE  | BPF_K:
	case BPF_JMP32 | BPF_JGT  | BPF_K:
	case BPF_JMP32 | BPF_JLT  | BPF_K:
	case BPF_JMP32 | BPF_JGE  | BPF_K:
	case BPF_JMP32 | BPF_JLE  | BPF_K:
	case BPF_JMP32 | BPF_JSGT | BPF_K:
	case BPF_JMP32 | BPF_JSLT | BPF_K:
	case BPF_JMP32 | BPF_JSGE | BPF_K:
	case BPF_JMP32 | BPF_JSLE | BPF_K:
	case BPF_JMP32 | BPF_JSET | BPF_K:
		/* Accommodate for extra offset in case of a backjump. */
		off = from->off;
		if (off < 0)
			off -= 2;
		*to++ = BPF_ALU32_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU32_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_JMP32_REG(from->code, from->dst_reg, BPF_REG_AX,
				      off);
		break;

	case BPF_LD | BPF_IMM | BPF_DW:
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ aux[1].imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_ALU64_IMM(BPF_LSH, BPF_REG_AX, 32);
		*to++ = BPF_ALU64_REG(BPF_MOV, aux[0].dst_reg, BPF_REG_AX);
		break;
	case 0: /* Part 2 of BPF_LD | BPF_IMM | BPF_DW. */
		*to++ = BPF_ALU32_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ aux[0].imm);
		*to++ = BPF_ALU32_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		if (emit_zext)
			*to++ = BPF_ZEXT_REG(BPF_REG_AX);
		*to++ = BPF_ALU64_REG(BPF_OR,  aux[0].dst_reg, BPF_REG_AX);
		break;

	case BPF_ST | BPF_MEM | BPF_DW:
	case BPF_ST | BPF_MEM | BPF_W:
	case BPF_ST | BPF_MEM | BPF_H:
	case BPF_ST | BPF_MEM | BPF_B:
		*to++ = BPF_ALU64_IMM(BPF_MOV, BPF_REG_AX, imm_rnd ^ from->imm);
		*to++ = BPF_ALU64_IMM(BPF_XOR, BPF_REG_AX, imm_rnd);
		*to++ = BPF_STX_MEM(from->code, from->dst_reg, BPF_REG_AX, from->off);
		break;
	}
out:
	return to - to_buff;
}

static struct bpf_prog *bpf_prog_clone_create(struct bpf_prog *fp_other,
					      gfp_t gfp_extra_flags)
{
	gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | gfp_extra_flags;
	struct bpf_prog *fp;

	fp = __vmalloc(fp_other->pages * PAGE_SIZE, gfp_flags);
	if (fp != NULL) {
		/* aux->prog still points to the fp_other one, so
		 * when promoting the clone to the real program,
		 * this still needs to be adapted.
		 */
		memcpy(fp, fp_other, fp_other->pages * PAGE_SIZE);
	}

	return fp;
}

static void bpf_prog_clone_free(struct bpf_prog *fp)
{
	/* aux was stolen by the other clone, so we cannot free
	 * it from this path! It will be freed eventually by the
	 * other program on release.
	 *
	 * At this point, we don't need a deferred release since
	 * clone is guaranteed to not be locked.
	 */
	fp->aux = NULL;
	fp->stats = NULL;
	fp->active = NULL;
	__bpf_prog_free(fp);
}

void bpf_jit_prog_release_other(struct bpf_prog *fp, struct bpf_prog *fp_other)
{
	/* We have to repoint aux->prog to self, as we don't
	 * know whether fp here is the clone or the original.
	 */
	fp->aux->prog = fp;
	bpf_prog_clone_free(fp_other);
}

struct bpf_prog *bpf_jit_blind_constants(struct bpf_prog *prog)
{
	struct bpf_insn insn_buff[16], aux[2];
	struct bpf_prog *clone, *tmp;
	int insn_delta, insn_cnt;
	struct bpf_insn *insn;
	int i, rewritten;

	if (!prog->blinding_requested || prog->blinded)
		return prog;

	clone = bpf_prog_clone_create(prog, GFP_USER);
	if (!clone)
		return ERR_PTR(-ENOMEM);

	insn_cnt = clone->len;
	insn = clone->insnsi;

	for (i = 0; i < insn_cnt; i++, insn++) {
		if (bpf_pseudo_func(insn)) {
			/* ld_imm64 with an address of bpf subprog is not
			 * a user controlled constant. Don't randomize it,
			 * since it will conflict with jit_subprogs() logic.
			 */
			insn++;
			i++;
			continue;
		}

		/* We temporarily need to hold the original ld64 insn
		 * so that we can still access the first part in the
		 * second blinding run.
		 */
		if (insn[0].code == (BPF_LD | BPF_IMM | BPF_DW) &&
		    insn[1].code == 0)
			memcpy(aux, insn, sizeof(aux));

		rewritten = bpf_jit_blind_insn(insn, aux, insn_buff,
						clone->aux->verifier_zext);
		if (!rewritten)
			continue;

		tmp = bpf_patch_insn_single(clone, i, insn_buff, rewritten);
		if (IS_ERR(tmp)) {
			/* Patching may have repointed aux->prog during
			 * realloc from the original one, so we need to
			 * fix it up here on error.
			 */
			bpf_jit_prog_release_other(prog, clone);
			return tmp;
		}

		clone = tmp;
		insn_delta = rewritten - 1;

		/* Walk new program and skip insns we just inserted. */
		insn = clone->insnsi + i + insn_delta;
		insn_cnt += insn_delta;
		i        += insn_delta;
	}

	clone->blinded = 1;
	return clone;
}
#endif /* CONFIG_BPF_JIT */

/* Base function for offset calculation. Needs to go into .text section,
 * therefore keeping it non-static as well; will also be used by JITs
 * anyway later on, so do not let the compiler omit it. This also needs
 * to go into kallsyms for correlation from e.g. bpftool, so naming
 * must not change.
 */
noinline u64 __bpf_call_base(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5)
{
	return 0;
}
EXPORT_SYMBOL_GPL(__bpf_call_base);

/* All UAPI available opcodes. */
#define BPF_INSN_MAP(INSN_2, INSN_3)		\
	/* 32 bit ALU operations. */		\
	/*   Register based. */			\
	INSN_3(ALU, ADD,  X),			\
	INSN_3(ALU, SUB,  X),			\
	INSN_3(ALU, AND,  X),			\
	INSN_3(ALU, OR,   X),			\
	INSN_3(ALU, LSH,  X),			\
	INSN_3(ALU, RSH,  X),			\
	INSN_3(ALU, XOR,  X),			\
	INSN_3(ALU, MUL,  X),			\
	INSN_3(ALU, MOV,  X),			\
	INSN_3(ALU, ARSH, X),			\
	INSN_3(ALU, DIV,  X),			\
	INSN_3(ALU, MOD,  X),			\
	INSN_2(ALU, NEG),			\
	INSN_3(ALU, END, TO_BE),		\
	INSN_3(ALU, END, TO_LE),		\
	/*   Immediate based. */		\
	INSN_3(ALU, ADD,  K),			\
	INSN_3(ALU, SUB,  K),			\
	INSN_3(ALU, AND,  K),			\
	INSN_3(ALU, OR,   K),			\
	INSN_3(ALU, LSH,  K),			\
	INSN_3(ALU, RSH,  K),			\
	INSN_3(ALU, XOR,  K),			\
	INSN_3(ALU, MUL,  K),			\
	INSN_3(ALU, MOV,  K),			\
	INSN_3(ALU, ARSH, K),			\
	INSN_3(ALU, DIV,  K),			\
	INSN_3(ALU, MOD,  K),			\
	/* 64 bit ALU operations. */		\
	/*   Register based. */			\
	INSN_3(ALU64, ADD,  X),			\
	INSN_3(ALU64, SUB,  X),			\
	INSN_3(ALU64, AND,  X),			\
	INSN_3(ALU64, OR,   X),			\
	INSN_3(ALU64, LSH,  X),			\
	INSN_3(ALU64, RSH,  X),			\
	INSN_3(ALU64, XOR,  X),			\
	INSN_3(ALU64, MUL,  X),			\
	INSN_3(ALU64, MOV,  X),			\
	INSN_3(ALU64, ARSH, X),			\
	INSN_3(ALU64, DIV,  X),			\
	INSN_3(ALU64, MOD,  X),			\
	INSN_2(ALU64, NEG),			\
	INSN_3(ALU64, END, TO_LE),		\
	/*   Immediate based. */		\
	INSN_3(ALU64, ADD,  K),			\
	INSN_3(ALU64, SUB,  K),			\
	INSN_3(ALU64, AND,  K),			\
	INSN_3(ALU64, OR,   K),			\
	INSN_3(ALU64, LSH,  K),			\
	INSN_3(ALU64, RSH,  K),			\
	INSN_3(ALU64, XOR,  K),			\
	INSN_3(ALU64, MUL,  K),			\
	INSN_3(ALU64, MOV,  K),			\
	INSN_3(ALU64, ARSH, K),			\
	INSN_3(ALU64, DIV,  K),			\
	INSN_3(ALU64, MOD,  K),			\
	/* Call instruction. */			\
	INSN_2(JMP, CALL),			\
	/* Exit instruction. */			\
	INSN_2(JMP, EXIT),			\
	/* 32-bit Jump instructions. */		\
	/*   Register based. */			\
	INSN_3(JMP32, JEQ,  X),			\
	INSN_3(JMP32, JNE,  X),			\
	INSN_3(JMP32, JGT,  X),			\
	INSN_3(JMP32, JLT,  X),			\
	INSN_3(JMP32, JGE,  X),			\
	INSN_3(JMP32, JLE,  X),			\
	INSN_3(JMP32, JSGT, X),			\
	INSN_3(JMP32, JSLT, X),			\
	INSN_3(JMP32, JSGE, X),			\
	INSN_3(JMP32, JSLE, X),			\
	INSN_3(JMP32, JSET, X),			\
	/*   Immediate based. */		\
	INSN_3(JMP32, JEQ,  K),			\
	INSN_3(JMP32, JNE,  K),			\
	INSN_3(JMP32, JGT,  K),			\
	INSN_3(JMP32, JLT,  K),			\
	INSN_3(JMP32, JGE,  K),			\
	INSN_3(JMP32, JLE,  K),			\
	INSN_3(JMP32, JSGT, K),			\
	INSN_3(JMP32, JSLT, K),			\
	INSN_3(JMP32, JSGE, K),			\
	INSN_3(JMP32, JSLE, K),			\
	INSN_3(JMP32, JSET, K),			\
	/* Jump instructions. */		\
	/*   Register based. */			\
	INSN_3(JMP, JEQ,  X),			\
	INSN_3(JMP, JNE,  X),			\
	INSN_3(JMP, JGT,  X),			\
	INSN_3(JMP, JLT,  X),			\
	INSN_3(JMP, JGE,  X),			\
	INSN_3(JMP, JLE,  X),			\
	INSN_3(JMP, JSGT, X),			\
	INSN_3(JMP, JSLT, X),			\
	INSN_3(JMP, JSGE, X),			\
	INSN_3(JMP, JSLE, X),			\
	INSN_3(JMP, JSET, X),			\
	/*   Immediate based. */		\
	INSN_3(JMP, JEQ,  K),			\
	INSN_3(JMP, JNE,  K),			\
	INSN_3(JMP, JGT,  K),			\
	INSN_3(JMP, JLT,  K),			\
	INSN_3(JMP, JGE,  K),			\
	INSN_3(JMP, JLE,  K),			\
	INSN_3(JMP, JSGT, K),			\
	INSN_3(JMP, JSLT, K),			\
	INSN_3(JMP, JSGE, K),			\
	INSN_3(JMP, JSLE, K),			\
	INSN_3(JMP, JSET, K),			\
	INSN_2(JMP, JA),			\
	INSN_2(JMP32, JA),			\
	/* Atomic operations. */		\
	INSN_3(STX, ATOMIC, B),			\
	INSN_3(STX, ATOMIC, H),			\
	INSN_3(STX, ATOMIC, W),			\
	INSN_3(STX, ATOMIC, DW),		\
	/* Store instructions. */		\
	/*   Register based. */			\
	INSN_3(STX, MEM,  B),			\
	INSN_3(STX, MEM,  H),			\
	INSN_3(STX, MEM,  W),			\
	INSN_3(STX, MEM,  DW),			\
	/*   Immediate based. */		\
	INSN_3(ST, MEM, B),			\
	INSN_3(ST, MEM, H),			\
	INSN_3(ST, MEM, W),			\
	INSN_3(ST, MEM, DW),			\
	/* Load instructions. */		\
	/*   Register based. */			\
	INSN_3(LDX, MEM, B),			\
	INSN_3(LDX, MEM, H),			\
	INSN_3(LDX, MEM, W),			\
	INSN_3(LDX, MEM, DW),			\
	INSN_3(LDX, MEMSX, B),			\
	INSN_3(LDX, MEMSX, H),			\
	INSN_3(LDX, MEMSX, W),			\
	/*   Immediate based. */		\
	INSN_3(LD, IMM, DW)

bool bpf_opcode_in_insntable(u8 code)
{
#define BPF_INSN_2_TBL(x, y)    [BPF_##x | BPF_##y] = true
#define BPF_INSN_3_TBL(x, y, z) [BPF_##x | BPF_##y | BPF_##z] = true
	static const bool public_insntable[256] = {
		[0 ... 255] = false,
		/* Now overwrite non-defaults ... */
		BPF_INSN_MAP(BPF_INSN_2_TBL, BPF_INSN_3_TBL),
		/* UAPI exposed, but rewritten opcodes. cBPF carry-over. */
		[BPF_LD | BPF_ABS | BPF_B] = true,
		[BPF_LD | BPF_ABS | BPF_H] = true,
		[BPF_LD | BPF_ABS | BPF_W] = true,
		[BPF_LD | BPF_IND | BPF_B] = true,
		[BPF_LD | BPF_IND | BPF_H] = true,
		[BPF_LD | BPF_IND | BPF_W] = true,
		[BPF_JMP | BPF_JCOND] = true,
	};
#undef BPF_INSN_3_TBL
#undef BPF_INSN_2_TBL
	return public_insntable[code];
}

#ifndef CONFIG_BPF_JIT_ALWAYS_ON
/**
 *	___bpf_prog_run - run eBPF program on a given context
 *	@regs: is the array of MAX_BPF_EXT_REG eBPF pseudo-registers
 *	@insn: is the array of eBPF instructions
 *
 * Decode and execute eBPF instructions.
 *
 * Return: whatever value is in %BPF_R0 at program exit
 */
static u64 ___bpf_prog_run(u64 *regs, const struct bpf_insn *insn)
{
#define BPF_INSN_2_LBL(x, y)    [BPF_##x | BPF_##y] = &&x##_##y
#define BPF_INSN_3_LBL(x, y, z) [BPF_##x | BPF_##y | BPF_##z] = &&x##_##y##_##z
	static const void * const jumptable[256] __annotate_jump_table = {
		[0 ... 255] = &&default_label,
		/* Now overwrite non-defaults ... */
		BPF_INSN_MAP(BPF_INSN_2_LBL, BPF_INSN_3_LBL),
		/* Non-UAPI available opcodes. */
		[BPF_JMP | BPF_CALL_ARGS] = &&JMP_CALL_ARGS,
		[BPF_JMP | BPF_TAIL_CALL] = &&JMP_TAIL_CALL,
		[BPF_ST  | BPF_NOSPEC] = &&ST_NOSPEC,
		[BPF_LDX | BPF_PROBE_MEM | BPF_B] = &&LDX_PROBE_MEM_B,
		[BPF_LDX | BPF_PROBE_MEM | BPF_H] = &&LDX_PROBE_MEM_H,
		[BPF_LDX | BPF_PROBE_MEM | BPF_W] = &&LDX_PROBE_MEM_W,
		[BPF_LDX | BPF_PROBE_MEM | BPF_DW] = &&LDX_PROBE_MEM_DW,
		[BPF_LDX | BPF_PROBE_MEMSX | BPF_B] = &&LDX_PROBE_MEMSX_B,
		[BPF_LDX | BPF_PROBE_MEMSX | BPF_H] = &&LDX_PROBE_MEMSX_H,
		[BPF_LDX | BPF_PROBE_MEMSX | BPF_W] = &&LDX_PROBE_MEMSX_W,
	};
#undef BPF_INSN_3_LBL
#undef BPF_INSN_2_LBL
	u32 tail_call_cnt = 0;

#define CONT	 ({ insn++; goto select_insn; })
#define CONT_JMP ({ insn++; goto select_insn; })

select_insn:
	goto *jumptable[insn->code];

	/* Explicitly mask the register-based shift amounts with 63 or 31
	 * to avoid undefined behavior. Normally this won't affect the
	 * generated code, for example, in case of native 64 bit archs such
	 * as x86-64 or arm64, the compiler is optimizing the AND away for
	 * the interpreter. In case of JITs, each of the JIT backends compiles
	 * the BPF shift operations to machine instructions which produce
	 * implementation-defined results in such a case; the resulting
	 * contents of the register may be arbitrary, but program behaviour
	 * as a whole remains defined. In other words, in case of JIT backends,
	 * the AND must /not/ be added to the emitted LSH/RSH/ARSH translation.
	 */
	/* ALU (shifts) */
#define SHT(OPCODE, OP)					\
	ALU64_##OPCODE##_X:				\
		DST = DST OP (SRC & 63);		\
		CONT;					\
	ALU_##OPCODE##_X:				\
		DST = (u32) DST OP ((u32) SRC & 31);	\
		CONT;					\
	ALU64_##OPCODE##_K:				\
		DST = DST OP IMM;			\
		CONT;					\
	ALU_##OPCODE##_K:				\
		DST = (u32) DST OP (u32) IMM;		\
		CONT;
	/* ALU (rest) */
#define ALU(OPCODE, OP)					\
	ALU64_##OPCODE##_X:				\
		DST = DST OP SRC;			\
		CONT;					\
	ALU_##OPCODE##_X:				\
		DST = (u32) DST OP (u32) SRC;		\
		CONT;					\
	ALU64_##OPCODE##_K:				\
		DST = DST OP IMM;			\
		CONT;					\
	ALU_##OPCODE##_K:				\
		DST = (u32) DST OP (u32) IMM;		\
		CONT;
	ALU(ADD,  +)
	ALU(SUB,  -)
	ALU(AND,  &)
	ALU(OR,   |)
	ALU(XOR,  ^)
	ALU(MUL,  *)
	SHT(LSH, <<)
	SHT(RSH, >>)
#undef SHT
#undef ALU
	ALU_NEG:
		DST = (u32) -DST;
		CONT;
	ALU64_NEG:
		DST = -DST;
		CONT;
	ALU_MOV_X:
		switch (OFF) {
		case 0:
			DST = (u32) SRC;
			break;
		case 8:
			DST = (u32)(s8) SRC;
			break;
		case 16:
			DST = (u32)(s16) SRC;
			break;
		}
		CONT;
	ALU_MOV_K:
		DST = (u32) IMM;
		CONT;
	ALU64_MOV_X:
		switch (OFF) {
		case 0:
			DST = SRC;
			break;
		case 8:
			DST = (s8) SRC;
			break;
		case 16:
			DST = (s16) SRC;
			break;
		case 32:
			DST = (s32) SRC;
			break;
		}
		CONT;
	ALU64_MOV_K:
		DST = IMM;
		CONT;
	LD_IMM_DW:
		DST = (u64) (u32) insn[0].imm | ((u64) (u32) insn[1].imm) << 32;
		insn++;
		CONT;
	ALU_ARSH_X:
		DST = (u64) (u32) (((s32) DST) >> (SRC & 31));
		CONT;
	ALU_ARSH_K:
		DST = (u64) (u32) (((s32) DST) >> IMM);
		CONT;
	ALU64_ARSH_X:
		(*(s64 *) &DST) >>= (SRC & 63);
		CONT;
	ALU64_ARSH_K:
		(*(s64 *) &DST) >>= IMM;
		CONT;
	ALU64_MOD_X:
		switch (OFF) {
		case 0:
			div64_u64_rem(DST, SRC, &AX);
			DST = AX;
			break;
		case 1:
			AX = div64_s64(DST, SRC);
			DST = DST - AX * SRC;
			break;
		}
		CONT;
	ALU_MOD_X:
		switch (OFF) {
		case 0:
			AX = (u32) DST;
			DST = do_div(AX, (u32) SRC);
			break;
		case 1:
			AX = abs((s32)DST);
			AX = do_div(AX, abs((s32)SRC));
			if ((s32)DST < 0)
				DST = (u32)-AX;
			else
				DST = (u32)AX;
			break;
		}
		CONT;
	ALU64_MOD_K:
		switch (OFF) {
		case 0:
			div64_u64_rem(DST, IMM, &AX);
			DST = AX;
			break;
		case 1:
			AX = div64_s64(DST, IMM);
			DST = DST - AX * IMM;
			break;
		}
		CONT;
	ALU_MOD_K:
		switch (OFF) {
		case 0:
			AX = (u32) DST;
			DST = do_div(AX, (u32) IMM);
			break;
		case 1:
			AX = abs((s32)DST);
			AX = do_div(AX, abs((s32)IMM));
			if ((s32)DST < 0)
				DST = (u32)-AX;
			else
				DST = (u32)AX;
			break;
		}
		CONT;
	ALU64_DIV_X:
		switch (OFF) {
		case 0:
			DST = div64_u64(DST, SRC);
			break;
		case 1:
			DST = div64_s64(DST, SRC);
			break;
		}
		CONT;
	ALU_DIV_X:
		switch (OFF) {
		case 0:
			AX = (u32) DST;
			do_div(AX, (u32) SRC);
			DST = (u32) AX;
			break;
		case 1:
			AX = abs((s32)DST);
			do_div(AX, abs((s32)SRC));
			if (((s32)DST < 0) == ((s32)SRC < 0))
				DST = (u32)AX;
			else
				DST = (u32)-AX;
			break;
		}
		CONT;
	ALU64_DIV_K:
		switch (OFF) {
		case 0:
			DST = div64_u64(DST, IMM);
			break;
		case 1:
			DST = div64_s64(DST, IMM);
			break;
		}
		CONT;
	ALU_DIV_K:
		switch (OFF) {
		case 0:
			AX = (u32) DST;
			do_div(AX, (u32) IMM);
			DST = (u32) AX;
			break;
		case 1:
			AX = abs((s32)DST);
			do_div(AX, abs((s32)IMM));
			if (((s32)DST < 0) == ((s32)IMM < 0))
				DST = (u32)AX;
			else
				DST = (u32)-AX;
			break;
		}
		CONT;
	ALU_END_TO_BE:
		switch (IMM) {
		case 16:
			DST = (__force u16) cpu_to_be16(DST);
			break;
		case 32:
			DST = (__force u32) cpu_to_be32(DST);
			break;
		case 64:
			DST = (__force u64) cpu_to_be64(DST);
			break;
		}
		CONT;
	ALU_END_TO_LE:
		switch (IMM) {
		case 16:
			DST = (__force u16) cpu_to_le16(DST);
			break;
		case 32:
			DST = (__force u32) cpu_to_le32(DST);
			break;
		case 64:
			DST = (__force u64) cpu_to_le64(DST);
			break;
		}
		CONT;
	ALU64_END_TO_LE:
		switch (IMM) {
		case 16:
			DST = (__force u16) __swab16(DST);
			break;
		case 32:
			DST = (__force u32) __swab32(DST);
			break;
		case 64:
			DST = (__force u64) __swab64(DST);
			break;
		}
		CONT;

	/* CALL */
	JMP_CALL:
		/* Function call scratches BPF_R1-BPF_R5 registers,
		 * preserves BPF_R6-BPF_R9, and stores return value
		 * into BPF_R0.
		 */
		BPF_R0 = (__bpf_call_base + insn->imm)(BPF_R1, BPF_R2, BPF_R3,
						       BPF_R4, BPF_R5);
		CONT;

	JMP_CALL_ARGS:
		BPF_R0 = (__bpf_call_base_args + insn->imm)(BPF_R1, BPF_R2,
							    BPF_R3, BPF_R4,
							    BPF_R5,
							    insn + insn->off + 1);
		CONT;

	JMP_TAIL_CALL: {
		struct bpf_map *map = (struct bpf_map *) (unsigned long) BPF_R2;
		struct bpf_array *array = container_of(map, struct bpf_array, map);
		struct bpf_prog *prog;
		u32 index = BPF_R3;

		if (unlikely(index >= array->map.max_entries))
			goto out;

		if (unlikely(tail_call_cnt >= MAX_TAIL_CALL_CNT))
			goto out;

		tail_call_cnt++;

		prog = READ_ONCE(array->ptrs[index]);
		if (!prog)
			goto out;

		/* ARG1 at this point is guaranteed to point to CTX from
		 * the verifier side due to the fact that the tail call is
		 * handled like a helper, that is, bpf_tail_call_proto,
		 * where arg1_type is ARG_PTR_TO_CTX.
		 */
		insn = prog->insnsi;
		goto select_insn;
out:
		CONT;
	}
	JMP_JA:
		insn += insn->off;
		CONT;
	JMP32_JA:
		insn += insn->imm;
		CONT;
	JMP_EXIT:
		return BPF_R0;
	/* JMP */
#define COND_JMP(SIGN, OPCODE, CMP_OP)				\
	JMP_##OPCODE##_X:					\
		if ((SIGN##64) DST CMP_OP (SIGN##64) SRC) {	\
			insn += insn->off;			\
			CONT_JMP;				\
		}						\
		CONT;						\
	JMP32_##OPCODE##_X:					\
		if ((SIGN##32) DST CMP_OP (SIGN##32) SRC) {	\
			insn += insn->off;			\
			CONT_JMP;				\
		}						\
		CONT;						\
	JMP_##OPCODE##_K:					\
		if ((SIGN##64) DST CMP_OP (SIGN##64) IMM) {	\
			insn += insn->off;			\
			CONT_JMP;				\
		}						\
		CONT;						\
	JMP32_##OPCODE##_K:					\
		if ((SIGN##32) DST CMP_OP (SIGN##32) IMM) {	\
			insn += insn->off;			\
			CONT_JMP;				\
		}						\
		CONT;
	COND_JMP(u, JEQ, ==)
	COND_JMP(u, JNE, !=)
	COND_JMP(u, JGT, >)
	COND_JMP(u, JLT, <)
	COND_JMP(u, JGE, >=)
	COND_JMP(u, JLE, <=)
	COND_JMP(u, JSET, &)
	COND_JMP(s, JSGT, >)
	COND_JMP(s, JSLT, <)
	COND_JMP(s, JSGE, >=)
	COND_JMP(s, JSLE, <=)
#undef COND_JMP
	/* ST, STX and LDX*/
	ST_NOSPEC:
		/* Speculation barrier for mitigating Speculative Store Bypass.
		 * In case of arm64, we rely on the firmware mitigation as
		 * controlled via the ssbd kernel parameter. Whenever the
		 * mitigation is enabled, it works for all of the kernel code
		 * with no need to provide any additional instructions here.
		 * In case of x86, we use 'lfence' insn for mitigation. We
		 * reuse preexisting logic from Spectre v1 mitigation that
		 * happens to produce the required code on x86 for v4 as well.
		 */
		barrier_nospec();
		CONT;
#define LDST(SIZEOP, SIZE)						\
	STX_MEM_##SIZEOP:						\
		*(SIZE *)(unsigned long) (DST + insn->off) = SRC;	\
		CONT;							\
	ST_MEM_##SIZEOP:						\
		*(SIZE *)(unsigned long) (DST + insn->off) = IMM;	\
		CONT;							\
	LDX_MEM_##SIZEOP:						\
		DST = *(SIZE *)(unsigned long) (SRC + insn->off);	\
		CONT;							\
	LDX_PROBE_MEM_##SIZEOP:						\
		bpf_probe_read_kernel_common(&DST, sizeof(SIZE),	\
			      (const void *)(long) (SRC + insn->off));	\
		DST = *((SIZE *)&DST);					\
		CONT;

	LDST(B,   u8)
	LDST(H,  u16)
	LDST(W,  u32)
	LDST(DW, u64)
#undef LDST

#define LDSX(SIZEOP, SIZE)						\
	LDX_MEMSX_##SIZEOP:						\
		DST = *(SIZE *)(unsigned long) (SRC + insn->off);	\
		CONT;							\
	LDX_PROBE_MEMSX_##SIZEOP:					\
		bpf_probe_read_kernel_common(&DST, sizeof(SIZE),		\
				      (const void *)(long) (SRC + insn->off));	\
		DST = *((SIZE *)&DST);					\
		CONT;

	LDSX(B,   s8)
	LDSX(H,  s16)
	LDSX(W,  s32)
#undef LDSX

#define ATOMIC_ALU_OP(BOP, KOP)						\
		case BOP:						\
			if (BPF_SIZE(insn->code) == BPF_W)		\
				atomic_##KOP((u32) SRC, (atomic_t *)(unsigned long) \
					     (DST + insn->off));	\
			else if (BPF_SIZE(insn->code) == BPF_DW)	\
				atomic64_##KOP((u64) SRC, (atomic64_t *)(unsigned long) \
					       (DST + insn->off));	\
			else						\
				goto default_label;			\
			break;						\
		case BOP | BPF_FETCH:					\
			if (BPF_SIZE(insn->code) == BPF_W)		\
				SRC = (u32) atomic_fetch_##KOP(		\
					(u32) SRC,			\
					(atomic_t *)(unsigned long) (DST + insn->off)); \
			else if (BPF_SIZE(insn->code) == BPF_DW)	\
				SRC = (u64) atomic64_fetch_##KOP(	\
					(u64) SRC,			\
					(atomic64_t *)(unsigned long) (DST + insn->off)); \
			else						\
				goto default_label;			\
			break;

	STX_ATOMIC_DW:
	STX_ATOMIC_W:
	STX_ATOMIC_H:
	STX_ATOMIC_B:
		switch (IMM) {
		/* Atomic read-modify-write instructions support only W and DW
		 * size modifiers.
		 */
		ATOMIC_ALU_OP(BPF_ADD, add)
		ATOMIC_ALU_OP(BPF_AND, and)
		ATOMIC_ALU_OP(BPF_OR, or)
		ATOMIC_ALU_OP(BPF_XOR, xor)
#undef ATOMIC_ALU_OP

		case BPF_XCHG:
			if (BPF_SIZE(insn->code) == BPF_W)
				SRC = (u32) atomic_xchg(
					(atomic_t *)(unsigned long) (DST + insn->off),
					(u32) SRC);
			else if (BPF_SIZE(insn->code) == BPF_DW)
				SRC = (u64) atomic64_xchg(
					(atomic64_t *)(unsigned long) (DST + insn->off),
					(u64) SRC);
			else
				goto default_label;
			break;
		case BPF_CMPXCHG:
			if (BPF_SIZE(insn->code) == BPF_W)
				BPF_R0 = (u32) atomic_cmpxchg(
					(atomic_t *)(unsigned long) (DST + insn->off),
					(u32) BPF_R0, (u32) SRC);
			else if (BPF_SIZE(insn->code) == BPF_DW)
				BPF_R0 = (u64) atomic64_cmpxchg(
					(atomic64_t *)(unsigned long) (DST + insn->off),
					(u64) BPF_R0, (u64) SRC);
			else
				goto default_label;
			break;
		/* Atomic load and store instructions support all size
		 * modifiers.
		 */
		case BPF_LOAD_ACQ:
			switch (BPF_SIZE(insn->code)) {
#define LOAD_ACQUIRE(SIZEOP, SIZE)				\
			case BPF_##SIZEOP:			\
				DST = (SIZE)smp_load_acquire(	\
					(SIZE *)(unsigned long)(SRC + insn->off));	\
				break;
			LOAD_ACQUIRE(B,   u8)
			LOAD_ACQUIRE(H,  u16)
			LOAD_ACQUIRE(W,  u32)
#ifdef CONFIG_64BIT
			LOAD_ACQUIRE(DW, u64)
#endif
#undef LOAD_ACQUIRE
			default:
				goto default_label;
			}
			break;
		case BPF_STORE_REL:
			switch (BPF_SIZE(insn->code)) {
#define STORE_RELEASE(SIZEOP, SIZE)			\
			case BPF_##SIZEOP:		\
				smp_store_release(	\
					(SIZE *)(unsigned long)(DST + insn->off), (SIZE)SRC);	\
				break;
			STORE_RELEASE(B,   u8)
			STORE_RELEASE(H,  u16)
			STORE_RELEASE(W,  u32)
#ifdef CONFIG_64BIT
			STORE_RELEASE(DW, u64)
#endif
#undef STORE_RELEASE
			default:
				goto default_label;
			}
			break;

		default:
			goto default_label;
		}
		CONT;

	default_label:
		/* If we ever reach this, we have a bug somewhere. Die hard here
		 * instead of just returning 0; we could be somewhere in a subprog,
		 * so execution could continue otherwise which we do /not/ want.
		 *
		 * Note, verifier whitelists all opcodes in bpf_opcode_in_insntable().
		 */
		pr_warn("BPF interpreter: unknown opcode %02x (imm: 0x%x)\n",
			insn->code, insn->imm);
		BUG_ON(1);
		return 0;
}

#define PROG_NAME(stack_size) __bpf_prog_run##stack_size
#define DEFINE_BPF_PROG_RUN(stack_size) \
static unsigned int PROG_NAME(stack_size)(const void *ctx, const struct bpf_insn *insn) \
{ \
	u64 stack[stack_size / sizeof(u64)]; \
	u64 regs[MAX_BPF_EXT_REG] = {}; \
\
	kmsan_unpoison_memory(stack, sizeof(stack)); \
	FP = (u64) (unsigned long) &stack[ARRAY_SIZE(stack)]; \
	ARG1 = (u64) (unsigned long) ctx; \
	return ___bpf_prog_run(regs, insn); \
}

#define PROG_NAME_ARGS(stack_size) __bpf_prog_run_args##stack_size
#define DEFINE_BPF_PROG_RUN_ARGS(stack_size) \
static u64 PROG_NAME_ARGS(stack_size)(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5, \
				      const struct bpf_insn *insn) \
{ \
	u64 stack[stack_size / sizeof(u64)]; \
	u64 regs[MAX_BPF_EXT_REG]; \
\
	kmsan_unpoison_memory(stack, sizeof(stack)); \
	FP = (u64) (unsigned long) &stack[ARRAY_SIZE(stack)]; \
	BPF_R1 = r1; \
	BPF_R2 = r2; \
	BPF_R3 = r3; \
	BPF_R4 = r4; \
	BPF_R5 = r5; \
	return ___bpf_prog_run(regs, insn); \
}

#define EVAL1(FN, X) FN(X)
#define EVAL2(FN, X, Y...) FN(X) EVAL1(FN, Y)
#define EVAL3(FN, X, Y...) FN(X) EVAL2(FN, Y)
#define EVAL4(FN, X, Y...) FN(X) EVAL3(FN, Y)
#define EVAL5(FN, X, Y...) FN(X) EVAL4(FN, Y)
#define EVAL6(FN, X, Y...) FN(X) EVAL5(FN, Y)

EVAL6(DEFINE_BPF_PROG_RUN, 32, 64, 96, 128, 160, 192);
EVAL6(DEFINE_BPF_PROG_RUN, 224, 256, 288, 320, 352, 384);
EVAL4(DEFINE_BPF_PROG_RUN, 416, 448, 480, 512);

EVAL6(DEFINE_BPF_PROG_RUN_ARGS, 32, 64, 96, 128, 160, 192);
EVAL6(DEFINE_BPF_PROG_RUN_ARGS, 224, 256, 288, 320, 352, 384);
EVAL4(DEFINE_BPF_PROG_RUN_ARGS, 416, 448, 480, 512);

#define PROG_NAME_LIST(stack_size) PROG_NAME(stack_size),

static unsigned int (*interpreters[])(const void *ctx,
				      const struct bpf_insn *insn) = {
EVAL6(PROG_NAME_LIST, 32, 64, 96, 128, 160, 192)
EVAL6(PROG_NAME_LIST, 224, 256, 288, 320, 352, 384)
EVAL4(PROG_NAME_LIST, 416, 448, 480, 512)
};
#undef PROG_NAME_LIST
#define PROG_NAME_LIST(stack_size) PROG_NAME_ARGS(stack_size),
static __maybe_unused
u64 (*interpreters_args[])(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5,
			   const struct bpf_insn *insn) = {
EVAL6(PROG_NAME_LIST, 32, 64, 96, 128, 160, 192)
EVAL6(PROG_NAME_LIST, 224, 256, 288, 320, 352, 384)
EVAL4(PROG_NAME_LIST, 416, 448, 480, 512)
};
#undef PROG_NAME_LIST

#ifdef CONFIG_BPF_SYSCALL
void bpf_patch_call_args(struct bpf_insn *insn, u32 stack_depth)
{
	stack_depth = max_t(u32, stack_depth, 1);
	insn->off = (s16) insn->imm;
	insn->imm = interpreters_args[(round_up(stack_depth, 32) / 32) - 1] -
		__bpf_call_base_args;
	insn->code = BPF_JMP | BPF_CALL_ARGS;
}
#endif
#endif

static unsigned int __bpf_prog_ret0_warn(const void *ctx,
					 const struct bpf_insn *insn)
{
	/* If this handler ever gets executed, then BPF_JIT_ALWAYS_ON
	 * is not working properly, or interpreter is being used when
	 * prog->jit_requested is not 0, so warn about it!
	 */
	WARN_ON_ONCE(1);
	return 0;
}

static bool __bpf_prog_map_compatible(struct bpf_map *map,
				      const struct bpf_prog *fp)
{
	enum bpf_prog_type prog_type = resolve_prog_type(fp);
	bool ret;
	struct bpf_prog_aux *aux = fp->aux;

	if (fp->kprobe_override)
		return false;

	spin_lock(&map->owner.lock);
	if (!map->owner.type) {
		/* There's no owner yet where we could check for
		 * compatibility.
		 */
		map->owner.type  = prog_type;
		map->owner.jited = fp->jited;
		map->owner.xdp_has_frags = aux->xdp_has_frags;
		map->owner.attach_func_proto = aux->attach_func_proto;
		ret = true;
	} else {
		ret = map->owner.type  == prog_type &&
		      map->owner.jited == fp->jited &&
		      map->owner.xdp_has_frags == aux->xdp_has_frags;
		if (ret &&
		    map->owner.attach_func_proto != aux->attach_func_proto) {
			switch (prog_type) {
			case BPF_PROG_TYPE_TRACING:
			case BPF_PROG_TYPE_LSM:
			case BPF_PROG_TYPE_EXT:
			case BPF_PROG_TYPE_STRUCT_OPS:
				ret = false;
				break;
			default:
				break;
			}
		}
	}
	spin_unlock(&map->owner.lock);

	return ret;
}

bool bpf_prog_map_compatible(struct bpf_map *map, const struct bpf_prog *fp)
{
	/* XDP programs inserted into maps are not guaranteed to run on
	 * a particular netdev (and can run outside driver context entirely
	 * in the case of devmap and cpumap). Until device checks
	 * are implemented, prohibit adding dev-bound programs to program maps.
	 */
	if (bpf_prog_is_dev_bound(fp->aux))
		return false;

	return __bpf_prog_map_compatible(map, fp);
}

static int bpf_check_tail_call(const struct bpf_prog *fp)
{
	struct bpf_prog_aux *aux = fp->aux;
	int i, ret = 0;

	mutex_lock(&aux->used_maps_mutex);
	for (i = 0; i < aux->used_map_cnt; i++) {
		struct bpf_map *map = aux->used_maps[i];

		if (!map_type_contains_progs(map))
			continue;

		if (!__bpf_prog_map_compatible(map, fp)) {
			ret = -EINVAL;
			goto out;
		}
	}

out:
	mutex_unlock(&aux->used_maps_mutex);
	return ret;
}

static void bpf_prog_select_func(struct bpf_prog *fp)
{
#ifndef CONFIG_BPF_JIT_ALWAYS_ON
	u32 stack_depth = max_t(u32, fp->aux->stack_depth, 1);
	u32 idx = (round_up(stack_depth, 32) / 32) - 1;

	/* may_goto may cause stack size > 512, leading to idx out-of-bounds.
	 * But for non-JITed programs, we don't need bpf_func, so no bounds
	 * check needed.
	 */
	if (!fp->jit_requested &&
	    !WARN_ON_ONCE(idx >= ARRAY_SIZE(interpreters))) {
		fp->bpf_func = interpreters[idx];
	} else {
		fp->bpf_func = __bpf_prog_ret0_warn;
	}
#else
	fp->bpf_func = __bpf_prog_ret0_warn;
#endif
}

/**
 *	bpf_prog_select_runtime - select exec runtime for BPF program
 *	@fp: bpf_prog populated with BPF program
 *	@err: pointer to error variable
 *
 * Try to JIT eBPF program, if JIT is not available, use interpreter.
 * The BPF program will be executed via bpf_prog_run() function.
 *
 * Return: the &fp argument along with &err set to 0 for success or
 * a negative errno code on failure
 */
struct bpf_prog *bpf_prog_select_runtime(struct bpf_prog *fp, int *err)
{
	/* In case of BPF to BPF calls, verifier did all the prep
	 * work with regards to JITing, etc.
	 */
	bool jit_needed = fp->jit_requested;

	if (fp->bpf_func)
		goto finalize;

	if (IS_ENABLED(CONFIG_BPF_JIT_ALWAYS_ON) ||
	    bpf_prog_has_kfunc_call(fp))
		jit_needed = true;

	bpf_prog_select_func(fp);

	/* eBPF JITs can rewrite the program in case constant
	 * blinding is active. However, in case of error during
	 * blinding, bpf_int_jit_compile() must always return a
	 * valid program, which in this case would simply not
	 * be JITed, but falls back to the interpreter.
	 */
	if (!bpf_prog_is_offloaded(fp->aux)) {
		*err = bpf_prog_alloc_jited_linfo(fp);
		if (*err)
			return fp;

		fp = bpf_int_jit_compile(fp);
		bpf_prog_jit_attempt_done(fp);
		if (!fp->jited && jit_needed) {
			*err = -ENOTSUPP;
			return fp;
		}
	} else {
		*err = bpf_prog_offload_compile(fp);
		if (*err)
			return fp;
	}

finalize:
	*err = bpf_prog_lock_ro(fp);
	if (*err)
		return fp;

	/* The tail call compatibility check can only be done at
	 * this late stage as we need to determine, if we deal
	 * with JITed or non JITed program concatenations and not
	 * all eBPF JITs might immediately support all features.
	 */
	*err = bpf_check_tail_call(fp);

	return fp;
}
EXPORT_SYMBOL_GPL(bpf_prog_select_runtime);

static unsigned int __bpf_prog_ret1(const void *ctx,
				    const struct bpf_insn *insn)
{
	return 1;
}

static struct bpf_prog_dummy {
	struct bpf_prog prog;
} dummy_bpf_prog = {
	.prog = {
		.bpf_func = __bpf_prog_ret1,
	},
};

struct bpf_empty_prog_array bpf_empty_prog_array = {
	.null_prog = NULL,
};
EXPORT_SYMBOL(bpf_empty_prog_array);

struct bpf_prog_array *bpf_prog_array_alloc(u32 prog_cnt, gfp_t flags)
{
	struct bpf_prog_array *p;

	if (prog_cnt)
		p = kzalloc(struct_size(p, items, prog_cnt + 1), flags);
	else
		p = &bpf_empty_prog_array.hdr;

	return p;
}

void bpf_prog_array_free(struct bpf_prog_array *progs)
{
	if (!progs || progs == &bpf_empty_prog_array.hdr)
		return;
	kfree_rcu(progs, rcu);
}

static void __bpf_prog_array_free_sleepable_cb(struct rcu_head *rcu)
{
	struct bpf_prog_array *progs;

	/* If RCU Tasks Trace grace period implies RCU grace period, there is
	 * no need to call kfree_rcu(), just call kfree() directly.
	 */
	progs = container_of(rcu, struct bpf_prog_array, rcu);
	if (rcu_trace_implies_rcu_gp())
		kfree(progs);
	else
		kfree_rcu(progs, rcu);
}

void bpf_prog_array_free_sleepable(struct bpf_prog_array *progs)
{
	if (!progs || progs == &bpf_empty_prog_array.hdr)
		return;
	call_rcu_tasks_trace(&progs->rcu, __bpf_prog_array_free_sleepable_cb);
}

int bpf_prog_array_length(struct bpf_prog_array *array)
{
	struct bpf_prog_array_item *item;
	u32 cnt = 0;

	for (item = array->items; item->prog; item++)
		if (item->prog != &dummy_bpf_prog.prog)
			cnt++;
	return cnt;
}

bool bpf_prog_array_is_empty(struct bpf_prog_array *array)
{
	struct bpf_prog_array_item *item;

	for (item = array->items; item->prog; item++)
		if (item->prog != &dummy_bpf_prog.prog)
			return false;
	return true;
}

static bool bpf_prog_array_copy_core(struct bpf_prog_array *array,
				     u32 *prog_ids,
				     u32 request_cnt)
{
	struct bpf_prog_array_item *item;
	int i = 0;

	for (item = array->items; item->prog; item++) {
		if (item->prog == &dummy_bpf_prog.prog)
			continue;
		prog_ids[i] = item->prog->aux->id;
		if (++i == request_cnt) {
			item++;
			break;
		}
	}

	return !!(item->prog);
}

int bpf_prog_array_copy_to_user(struct bpf_prog_array *array,
				__u32 __user *prog_ids, u32 cnt)
{
	unsigned long err = 0;
	bool nospc;
	u32 *ids;

	/* users of this function are doing:
	 * cnt = bpf_prog_array_length();
	 * if (cnt > 0)
	 *     bpf_prog_array_copy_to_user(..., cnt);
	 * so below kcalloc doesn't need extra cnt > 0 check.
	 */
	ids = kcalloc(cnt, sizeof(u32), GFP_USER | __GFP_NOWARN);
	if (!ids)
		return -ENOMEM;
	nospc = bpf_prog_array_copy_core(array, ids, cnt);
	err = copy_to_user(prog_ids, ids, cnt * sizeof(u32));
	kfree(ids);
	if (err)
		return -EFAULT;
	if (nospc)
		return -ENOSPC;
	return 0;
}

void bpf_prog_array_delete_safe(struct bpf_prog_array *array,
				struct bpf_prog *old_prog)
{
	struct bpf_prog_array_item *item;

	for (item = array->items; item->prog; item++)
		if (item->prog == old_prog) {
			WRITE_ONCE(item->prog, &dummy_bpf_prog.prog);
			break;
		}
}

/**
 * bpf_prog_array_delete_safe_at() - Replaces the program at the given
 *                                   index into the program array with
 *                                   a dummy no-op program.
 * @array: a bpf_prog_array
 * @index: the index of the program to replace
 *
 * Skips over dummy programs, by not counting them, when calculating
 * the position of the program to replace.
 *
 * Return:
 * * 0		- Success
 * * -EINVAL	- Invalid index value. Must be a non-negative integer.
 * * -ENOENT	- Index out of range
 */
int bpf_prog_array_delete_safe_at(struct bpf_prog_array *array, int index)
{
	return bpf_prog_array_update_at(array, index, &dummy_bpf_prog.prog);
}

/**
 * bpf_prog_array_update_at() - Updates the program at the given index
 *                              into the program array.
 * @array: a bpf_prog_array
 * @index: the index of the program to update
 * @prog: the program to insert into the array
 *
 * Skips over dummy programs, by not counting them, when calculating
 * the position of the program to update.
 *
 * Return:
 * * 0		- Success
 * * -EINVAL	- Invalid index value. Must be a non-negative integer.
 * * -ENOENT	- Index out of range
 */
int bpf_prog_array_update_at(struct bpf_prog_array *array, int index,
			     struct bpf_prog *prog)
{
	struct bpf_prog_array_item *item;

	if (unlikely(index < 0))
		return -EINVAL;

	for (item = array->items; item->prog; item++) {
		if (item->prog == &dummy_bpf_prog.prog)
			continue;
		if (!index) {
			WRITE_ONCE(item->prog, prog);
			return 0;
		}
		index--;
	}
	return -ENOENT;
}

int bpf_prog_array_copy(struct bpf_prog_array *old_array,
			struct bpf_prog *exclude_prog,
			struct bpf_prog *include_prog,
			u64 bpf_cookie,
			struct bpf_prog_array **new_array)
{
	int new_prog_cnt, carry_prog_cnt = 0;
	struct bpf_prog_array_item *existing, *new;
	struct bpf_prog_array *array;
	bool found_exclude = false;

	/* Figure out how many existing progs we need to carry over to
	 * the new array.
	 */
	if (old_array) {
		existing = old_array->items;
		for (; existing->prog; existing++) {
			if (existing->prog == exclude_prog) {
				found_exclude = true;
				continue;
			}
			if (existing->prog != &dummy_bpf_prog.prog)
				carry_prog_cnt++;
			if (existing->prog == include_prog)
				return -EEXIST;
		}
	}

	if (exclude_prog && !found_exclude)
		return -ENOENT;

	/* How many progs (not NULL) will be in the new array? */
	new_prog_cnt = carry_prog_cnt;
	if (include_prog)
		new_prog_cnt += 1;

	/* Do we have any prog (not NULL) in the new array? */
	if (!new_prog_cnt) {
		*new_array = NULL;
		return 0;
	}

	/* +1 as the end of prog_array is marked with NULL */
	array = bpf_prog_array_alloc(new_prog_cnt + 1, GFP_KERNEL);
	if (!array)
		return -ENOMEM;
	new = array->items;

	/* Fill in the new prog array */
	if (carry_prog_cnt) {
		existing = old_array->items;
		for (; existing->prog; existing++) {
			if (existing->prog == exclude_prog ||
			    existing->prog == &dummy_bpf_prog.prog)
				continue;

			new->prog = existing->prog;
			new->bpf_cookie = existing->bpf_cookie;
			new++;
		}
	}
	if (include_prog) {
		new->prog = include_prog;
		new->bpf_cookie = bpf_cookie;
		new++;
	}
	new->prog = NULL;
	*new_array = array;
	return 0;
}

int bpf_prog_array_copy_info(struct bpf_prog_array *array,
			     u32 *prog_ids, u32 request_cnt,
			     u32 *prog_cnt)
{
	u32 cnt = 0;

	if (array)
		cnt = bpf_prog_array_length(array);

	*prog_cnt = cnt;

	/* return early if user requested only program count or nothing to copy */
	if (!request_cnt || !cnt)
		return 0;

	/* this function is called under trace/bpf_trace.c: bpf_event_mutex */
	return bpf_prog_array_copy_core(array, prog_ids, request_cnt) ? -ENOSPC
								     : 0;
}

void __bpf_free_used_maps(struct bpf_prog_aux *aux,
			  struct bpf_map **used_maps, u32 len)
{
	struct bpf_map *map;
	bool sleepable;
	u32 i;

	sleepable = aux->prog->sleepable;
	for (i = 0; i < len; i++) {
		map = used_maps[i];
		if (map->ops->map_poke_untrack)
			map->ops->map_poke_untrack(map, aux);
		if (sleepable)
			atomic64_dec(&map->sleepable_refcnt);
		bpf_map_put(map);
	}
}

static void bpf_free_used_maps(struct bpf_prog_aux *aux)
{
	__bpf_free_used_maps(aux, aux->used_maps, aux->used_map_cnt);
	kfree(aux->used_maps);
}

void __bpf_free_used_btfs(struct btf_mod_pair *used_btfs, u32 len)
{
#ifdef CONFIG_BPF_SYSCALL
	struct btf_mod_pair *btf_mod;
	u32 i;

	for (i = 0; i < len; i++) {
		btf_mod = &used_btfs[i];
		if (btf_mod->module)
			module_put(btf_mod->module);
		btf_put(btf_mod->btf);
	}
#endif
}

static void bpf_free_used_btfs(struct bpf_prog_aux *aux)
{
	__bpf_free_used_btfs(aux->used_btfs, aux->used_btf_cnt);
	kfree(aux->used_btfs);
}

static void bpf_prog_free_deferred(struct work_struct *work)
{
	struct bpf_prog_aux *aux;
	int i;

	aux = container_of(work, struct bpf_prog_aux, work);
#ifdef CONFIG_BPF_SYSCALL
	bpf_free_kfunc_btf_tab(aux->kfunc_btf_tab);
#endif
#ifdef CONFIG_CGROUP_BPF
	if (aux->cgroup_atype != CGROUP_BPF_ATTACH_TYPE_INVALID)
		bpf_cgroup_atype_put(aux->cgroup_atype);
#endif
	bpf_free_used_maps(aux);
	bpf_free_used_btfs(aux);
	if (bpf_prog_is_dev_bound(aux))
		bpf_prog_dev_bound_destroy(aux->prog);
#ifdef CONFIG_PERF_EVENTS
	if (aux->prog->has_callchain_buf)
		put_callchain_buffers();
#endif
	if (aux->dst_trampoline)
		bpf_trampoline_put(aux->dst_trampoline);
	for (i = 0; i < aux->real_func_cnt; i++) {
		/* We can just unlink the subprog poke descriptor table as
		 * it was originally linked to the main program and is also
		 * released along with it.
		 */
		aux->func[i]->aux->poke_tab = NULL;
		bpf_jit_free(aux->func[i]);
	}
	if (aux->real_func_cnt) {
		kfree(aux->func);
		bpf_prog_unlock_free(aux->prog);
	} else {
		bpf_jit_free(aux->prog);
	}
}

void bpf_prog_free(struct bpf_prog *fp)
{
	struct bpf_prog_aux *aux = fp->aux;

	if (aux->dst_prog)
		bpf_prog_put(aux->dst_prog);
	bpf_token_put(aux->token);
	INIT_WORK(&aux->work, bpf_prog_free_deferred);
	schedule_work(&aux->work);
}
EXPORT_SYMBOL_GPL(bpf_prog_free);

/* RNG for unprivileged user space with separated state from prandom_u32(). */
static DEFINE_PER_CPU(struct rnd_state, bpf_user_rnd_state);

void bpf_user_rnd_init_once(void)
{
	prandom_init_once(&bpf_user_rnd_state);
}

BPF_CALL_0(bpf_user_rnd_u32)
{
	/* Should someone ever have the rather unwise idea to use some
	 * of the registers passed into this function, then note that
	 * this function is called from native eBPF and classic-to-eBPF
	 * transformations. Register assignments from both sides are
	 * different, f.e. classic always sets fn(ctx, A, X) here.
	 */
	struct rnd_state *state;
	u32 res;

	state = &get_cpu_var(bpf_user_rnd_state);
	res = prandom_u32_state(state);
	put_cpu_var(bpf_user_rnd_state);

	return res;
}

BPF_CALL_0(bpf_get_raw_cpu_id)
{
	return raw_smp_processor_id();
}

/* Weak definitions of helper functions in case we don't have bpf syscall. */
const struct bpf_func_proto bpf_map_lookup_elem_proto __weak;
const struct bpf_func_proto bpf_map_update_elem_proto __weak;
const struct bpf_func_proto bpf_map_delete_elem_proto __weak;
const struct bpf_func_proto bpf_map_push_elem_proto __weak;
const struct bpf_func_proto bpf_map_pop_elem_proto __weak;
const struct bpf_func_proto bpf_map_peek_elem_proto __weak;
const struct bpf_func_proto bpf_map_lookup_percpu_elem_proto __weak;
const struct bpf_func_proto bpf_spin_lock_proto __weak;
const struct bpf_func_proto bpf_spin_unlock_proto __weak;
const struct bpf_func_proto bpf_jiffies64_proto __weak;

const struct bpf_func_proto bpf_get_prandom_u32_proto __weak;
const struct bpf_func_proto bpf_get_smp_processor_id_proto __weak;
const struct bpf_func_proto bpf_get_numa_node_id_proto __weak;
const struct bpf_func_proto bpf_ktime_get_ns_proto __weak;
const struct bpf_func_proto bpf_ktime_get_boot_ns_proto __weak;
const struct bpf_func_proto bpf_ktime_get_coarse_ns_proto __weak;
const struct bpf_func_proto bpf_ktime_get_tai_ns_proto __weak;

const struct bpf_func_proto bpf_get_current_pid_tgid_proto __weak;
const struct bpf_func_proto bpf_get_current_uid_gid_proto __weak;
const struct bpf_func_proto bpf_get_current_comm_proto __weak;
const struct bpf_func_proto bpf_get_current_cgroup_id_proto __weak;
const struct bpf_func_proto bpf_get_current_ancestor_cgroup_id_proto __weak;
const struct bpf_func_proto bpf_get_local_storage_proto __weak;
const struct bpf_func_proto bpf_get_ns_current_pid_tgid_proto __weak;
const struct bpf_func_proto bpf_snprintf_btf_proto __weak;
const struct bpf_func_proto bpf_seq_printf_btf_proto __weak;
const struct bpf_func_proto bpf_set_retval_proto __weak;
const struct bpf_func_proto bpf_get_retval_proto __weak;

const struct bpf_func_proto * __weak bpf_get_trace_printk_proto(void)
{
	return NULL;
}

const struct bpf_func_proto * __weak bpf_get_trace_vprintk_proto(void)
{
	return NULL;
}

const struct bpf_func_proto * __weak bpf_get_perf_event_read_value_proto(void)
{
	return NULL;
}

u64 __weak
bpf_event_output(struct bpf_map *map, u64 flags, void *meta, u64 meta_size,
		 void *ctx, u64 ctx_size, bpf_ctx_copy_t ctx_copy)
{
	return -ENOTSUPP;
}
EXPORT_SYMBOL_GPL(bpf_event_output);

/* Always built-in helper functions. */
const struct bpf_func_proto bpf_tail_call_proto = {
	.func		= NULL,
	.gpl_only	= false,
	.ret_type	= RET_VOID,
	.arg1_type	= ARG_PTR_TO_CTX,
	.arg2_type	= ARG_CONST_MAP_PTR,
	.arg3_type	= ARG_ANYTHING,
};

/* Stub for JITs that only support cBPF. eBPF programs are interpreted.
 * It is encouraged to implement bpf_int_jit_compile() instead, so that
 * eBPF and implicitly also cBPF can get JITed!
 */
struct bpf_prog * __weak bpf_int_jit_compile(struct bpf_prog *prog)
{
	return prog;
}

/* Stub for JITs that support eBPF. All cBPF code gets transformed into
 * eBPF by the kernel and is later compiled by bpf_int_jit_compile().
 */
void __weak bpf_jit_compile(struct bpf_prog *prog)
{
}

bool __weak bpf_helper_changes_pkt_data(enum bpf_func_id func_id)
{
	return false;
}

/* Return TRUE if the JIT backend wants verifier to enable sub-register usage
 * analysis code and wants explicit zero extension inserted by verifier.
 * Otherwise, return FALSE.
 *
 * The verifier inserts an explicit zero extension after BPF_CMPXCHGs even if
 * you don't override this. JITs that don't want these extra insns can detect
 * them using insn_is_zext.
 */
bool __weak bpf_jit_needs_zext(void)
{
	return false;
}

/* Return true if the JIT inlines the call to the helper corresponding to
 * the imm.
 *
 * The verifier will not patch the insn->imm for the call to the helper if
 * this returns true.
 */
bool __weak bpf_jit_inlines_helper_call(s32 imm)
{
	return false;
}

/* Return TRUE if the JIT backend supports mixing bpf2bpf and tailcalls. */
bool __weak bpf_jit_supports_subprog_tailcalls(void)
{
	return false;
}

bool __weak bpf_jit_supports_percpu_insn(void)
{
	return false;
}

bool __weak bpf_jit_supports_kfunc_call(void)
{
	return false;
}

bool __weak bpf_jit_supports_far_kfunc_call(void)
{
	return false;
}

bool __weak bpf_jit_supports_arena(void)
{
	return false;
}

bool __weak bpf_jit_supports_insn(struct bpf_insn *insn, bool in_arena)
{
	return false;
}

u64 __weak bpf_arch_uaddress_limit(void)
{
#if defined(CONFIG_64BIT) && defined(CONFIG_ARCH_HAS_NON_OVERLAPPING_ADDRESS_SPACE)
	return TASK_SIZE;
#else
	return 0;
#endif
}

/* Return TRUE if the JIT backend satisfies the following two conditions:
 * 1) JIT backend supports atomic_xchg() on pointer-sized words.
 * 2) Under the specific arch, the implementation of xchg() is the same
 *    as atomic_xchg() on pointer-sized words.
 */
bool __weak bpf_jit_supports_ptr_xchg(void)
{
	return false;
}

/* To execute LD_ABS/LD_IND instructions __bpf_prog_run() may call
 * skb_copy_bits(), so provide a weak definition of it for NET-less config.
 */
int __weak skb_copy_bits(const struct sk_buff *skb, int offset, void *to,
			 int len)
{
	return -EFAULT;
}

int __weak bpf_arch_text_poke(void *ip, enum bpf_text_poke_type t,
			      void *addr1, void *addr2)
{
	return -ENOTSUPP;
}

void * __weak bpf_arch_text_copy(void *dst, void *src, size_t len)
{
	return ERR_PTR(-ENOTSUPP);
}

int __weak bpf_arch_text_invalidate(void *dst, size_t len)
{
	return -ENOTSUPP;
}

bool __weak bpf_jit_supports_exceptions(void)
{
	return false;
}

bool __weak bpf_jit_supports_private_stack(void)
{
	return false;
}

void __weak arch_bpf_stack_walk(bool (*consume_fn)(void *cookie, u64 ip, u64 sp, u64 bp), void *cookie)
{
}

bool __weak bpf_jit_supports_timed_may_goto(void)
{
	return false;
}

u64 __weak arch_bpf_timed_may_goto(void)
{
	return 0;
}

u64 bpf_check_timed_may_goto(struct bpf_timed_may_goto *p)
{
	u64 time = ktime_get_mono_fast_ns();

	/* Populate the timestamp for this stack frame, and refresh count. */
	if (!p->timestamp) {
		p->timestamp = time;
		return BPF_MAX_TIMED_LOOPS;
	}
	/* Check if we've exhausted our time slice, and zero count. */
	if (time - p->timestamp >= (NSEC_PER_SEC / 4))
		return 0;
	/* Refresh the count for the stack frame. */
	return BPF_MAX_TIMED_LOOPS;
}

/* for configs without MMU or 32-bit */
__weak const struct bpf_map_ops arena_map_ops;
__weak u64 bpf_arena_get_user_vm_start(struct bpf_arena *arena)
{
	return 0;
}
__weak u64 bpf_arena_get_kern_vm_start(struct bpf_arena *arena)
{
	return 0;
}

#ifdef CONFIG_BPF_SYSCALL
static int __init bpf_global_ma_init(void)
{
	int ret;

	ret = bpf_mem_alloc_init(&bpf_global_ma, 0, false);
	bpf_global_ma_set = !ret;
	return ret;
}
late_initcall(bpf_global_ma_init);
#endif

DEFINE_STATIC_KEY_FALSE(bpf_stats_enabled_key);
EXPORT_SYMBOL(bpf_stats_enabled_key);

/* All definitions of tracepoints related to BPF. */
#define CREATE_TRACE_POINTS
#include <linux/bpf_trace.h>

EXPORT_TRACEPOINT_SYMBOL_GPL(xdp_exception);
EXPORT_TRACEPOINT_SYMBOL_GPL(xdp_bulk_tx);
