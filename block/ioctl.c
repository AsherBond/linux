// SPDX-License-Identifier: GPL-2.0
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/blkdev.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/blkpg.h>
#include <linux/hdreg.h>
#include <linux/backing-dev.h>
#include <linux/fs.h>
#include <linux/blktrace_api.h>
#include <linux/pr.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <linux/io_uring/cmd.h>
#include <linux/blk-integrity.h>
#include <uapi/linux/blkdev.h>
#include "blk.h"
#include "blk-crypto-internal.h"

static int blkpg_do_ioctl(struct block_device *bdev,
			  struct blkpg_partition __user *upart, int op)
{
	struct gendisk *disk = bdev->bd_disk;
	struct blkpg_partition p;
	sector_t start, length, capacity, end;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (copy_from_user(&p, upart, sizeof(struct blkpg_partition)))
		return -EFAULT;
	if (bdev_is_partition(bdev))
		return -EINVAL;

	if (p.pno <= 0)
		return -EINVAL;

	if (op == BLKPG_DEL_PARTITION)
		return bdev_del_partition(disk, p.pno);

	if (p.start < 0 || p.length <= 0 || LLONG_MAX - p.length < p.start)
		return -EINVAL;
	/* Check that the partition is aligned to the block size */
	if (!IS_ALIGNED(p.start | p.length, bdev_logical_block_size(bdev)))
		return -EINVAL;

	start = p.start >> SECTOR_SHIFT;
	length = p.length >> SECTOR_SHIFT;
	capacity = get_capacity(disk);

	if (check_add_overflow(start, length, &end))
		return -EINVAL;

	if (start >= capacity || end > capacity)
		return -EINVAL;

	switch (op) {
	case BLKPG_ADD_PARTITION:
		return bdev_add_partition(disk, p.pno, start, length);
	case BLKPG_RESIZE_PARTITION:
		return bdev_resize_partition(disk, p.pno, start, length);
	default:
		return -EINVAL;
	}
}

static int blkpg_ioctl(struct block_device *bdev,
		       struct blkpg_ioctl_arg __user *arg)
{
	struct blkpg_partition __user *udata;
	int op;

	if (get_user(op, &arg->op) || get_user(udata, &arg->data))
		return -EFAULT;

	return blkpg_do_ioctl(bdev, udata, op);
}

#ifdef CONFIG_COMPAT
struct compat_blkpg_ioctl_arg {
	compat_int_t op;
	compat_int_t flags;
	compat_int_t datalen;
	compat_caddr_t data;
};

static int compat_blkpg_ioctl(struct block_device *bdev,
			      struct compat_blkpg_ioctl_arg __user *arg)
{
	compat_caddr_t udata;
	int op;

	if (get_user(op, &arg->op) || get_user(udata, &arg->data))
		return -EFAULT;

	return blkpg_do_ioctl(bdev, compat_ptr(udata), op);
}
#endif

/*
 * Check that [start, start + len) is a valid range from the block device's
 * perspective, including verifying that it can be correctly translated into
 * logical block addresses.
 */
static int blk_validate_byte_range(struct block_device *bdev,
				   uint64_t start, uint64_t len)
{
	unsigned int bs_mask = bdev_logical_block_size(bdev) - 1;
	uint64_t end;

	if ((start | len) & bs_mask)
		return -EINVAL;
	if (!len)
		return -EINVAL;
	if (check_add_overflow(start, len, &end) || end > bdev_nr_bytes(bdev))
		return -EINVAL;

	return 0;
}

static int blk_ioctl_discard(struct block_device *bdev, blk_mode_t mode,
		unsigned long arg)
{
	uint64_t range[2], start, len;
	struct bio *prev = NULL, *bio;
	sector_t sector, nr_sects;
	struct blk_plug plug;
	int err;

	if (copy_from_user(range, (void __user *)arg, sizeof(range)))
		return -EFAULT;
	start = range[0];
	len = range[1];

	if (!bdev_max_discard_sectors(bdev))
		return -EOPNOTSUPP;

	if (!(mode & BLK_OPEN_WRITE))
		return -EBADF;
	if (bdev_read_only(bdev))
		return -EPERM;
	err = blk_validate_byte_range(bdev, start, len);
	if (err)
		return err;

	inode_lock(bdev->bd_mapping->host);
	filemap_invalidate_lock(bdev->bd_mapping);
	err = truncate_bdev_range(bdev, mode, start, start + len - 1);
	if (err)
		goto fail;

	sector = start >> SECTOR_SHIFT;
	nr_sects = len >> SECTOR_SHIFT;

	blk_start_plug(&plug);
	while (1) {
		if (fatal_signal_pending(current)) {
			if (prev)
				bio_await_chain(prev);
			err = -EINTR;
			goto out_unplug;
		}
		bio = blk_alloc_discard_bio(bdev, &sector, &nr_sects,
				GFP_KERNEL);
		if (!bio)
			break;
		prev = bio_chain_and_submit(prev, bio);
	}
	if (prev) {
		err = submit_bio_wait(prev);
		if (err == -EOPNOTSUPP)
			err = 0;
		bio_put(prev);
	}
out_unplug:
	blk_finish_plug(&plug);
fail:
	filemap_invalidate_unlock(bdev->bd_mapping);
	inode_unlock(bdev->bd_mapping->host);
	return err;
}

static int blk_ioctl_secure_erase(struct block_device *bdev, blk_mode_t mode,
		void __user *argp)
{
	uint64_t start, len, end;
	uint64_t range[2];
	int err;

	if (!(mode & BLK_OPEN_WRITE))
		return -EBADF;
	if (!bdev_max_secure_erase_sectors(bdev))
		return -EOPNOTSUPP;
	if (copy_from_user(range, argp, sizeof(range)))
		return -EFAULT;

	start = range[0];
	len = range[1];
	if ((start & 511) || (len & 511))
		return -EINVAL;
	if (check_add_overflow(start, len, &end) ||
	    end > bdev_nr_bytes(bdev))
		return -EINVAL;

	inode_lock(bdev->bd_mapping->host);
	filemap_invalidate_lock(bdev->bd_mapping);
	err = truncate_bdev_range(bdev, mode, start, end - 1);
	if (!err)
		err = blkdev_issue_secure_erase(bdev, start >> 9, len >> 9,
						GFP_KERNEL);
	filemap_invalidate_unlock(bdev->bd_mapping);
	inode_unlock(bdev->bd_mapping->host);
	return err;
}


static int blk_ioctl_zeroout(struct block_device *bdev, blk_mode_t mode,
		unsigned long arg)
{
	uint64_t range[2];
	uint64_t start, end, len;
	int err;

	if (!(mode & BLK_OPEN_WRITE))
		return -EBADF;

	if (copy_from_user(range, (void __user *)arg, sizeof(range)))
		return -EFAULT;

	start = range[0];
	len = range[1];
	end = start + len - 1;

	if (start & 511)
		return -EINVAL;
	if (len & 511)
		return -EINVAL;
	if (end >= (uint64_t)bdev_nr_bytes(bdev))
		return -EINVAL;
	if (end < start)
		return -EINVAL;

	/* Invalidate the page cache, including dirty pages */
	inode_lock(bdev->bd_mapping->host);
	filemap_invalidate_lock(bdev->bd_mapping);
	err = truncate_bdev_range(bdev, mode, start, end);
	if (err)
		goto fail;

	err = blkdev_issue_zeroout(bdev, start >> 9, len >> 9, GFP_KERNEL,
				   BLKDEV_ZERO_NOUNMAP | BLKDEV_ZERO_KILLABLE);

fail:
	filemap_invalidate_unlock(bdev->bd_mapping);
	inode_unlock(bdev->bd_mapping->host);
	return err;
}

static int put_ushort(unsigned short __user *argp, unsigned short val)
{
	return put_user(val, argp);
}

static int put_int(int __user *argp, int val)
{
	return put_user(val, argp);
}

static int put_uint(unsigned int __user *argp, unsigned int val)
{
	return put_user(val, argp);
}

static int put_long(long __user *argp, long val)
{
	return put_user(val, argp);
}

static int put_ulong(unsigned long __user *argp, unsigned long val)
{
	return put_user(val, argp);
}

static int put_u64(u64 __user *argp, u64 val)
{
	return put_user(val, argp);
}

#ifdef CONFIG_COMPAT
static int compat_put_long(compat_long_t __user *argp, long val)
{
	return put_user(val, argp);
}

static int compat_put_ulong(compat_ulong_t __user *argp, compat_ulong_t val)
{
	return put_user(val, argp);
}
#endif

#ifdef CONFIG_COMPAT
/*
 * This is the equivalent of compat_ptr_ioctl(), to be used by block
 * drivers that implement only commands that are completely compatible
 * between 32-bit and 64-bit user space
 */
int blkdev_compat_ptr_ioctl(struct block_device *bdev, blk_mode_t mode,
			unsigned cmd, unsigned long arg)
{
	struct gendisk *disk = bdev->bd_disk;

	if (disk->fops->ioctl)
		return disk->fops->ioctl(bdev, mode, cmd,
					 (unsigned long)compat_ptr(arg));

	return -ENOIOCTLCMD;
}
EXPORT_SYMBOL(blkdev_compat_ptr_ioctl);
#endif

static bool blkdev_pr_allowed(struct block_device *bdev, blk_mode_t mode)
{
	/* no sense to make reservations for partitions */
	if (bdev_is_partition(bdev))
		return false;

	if (capable(CAP_SYS_ADMIN))
		return true;
	/*
	 * Only allow unprivileged reservations if the file descriptor is open
	 * for writing.
	 */
	return mode & BLK_OPEN_WRITE;
}

static int blkdev_pr_register(struct block_device *bdev, blk_mode_t mode,
		struct pr_registration __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops;
	struct pr_registration reg;

	if (!blkdev_pr_allowed(bdev, mode))
		return -EPERM;
	if (!ops || !ops->pr_register)
		return -EOPNOTSUPP;
	if (copy_from_user(&reg, arg, sizeof(reg)))
		return -EFAULT;

	if (reg.flags & ~PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;
	return ops->pr_register(bdev, reg.old_key, reg.new_key, reg.flags);
}

static int blkdev_pr_reserve(struct block_device *bdev, blk_mode_t mode,
		struct pr_reservation __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops;
	struct pr_reservation rsv;

	if (!blkdev_pr_allowed(bdev, mode))
		return -EPERM;
	if (!ops || !ops->pr_reserve)
		return -EOPNOTSUPP;
	if (copy_from_user(&rsv, arg, sizeof(rsv)))
		return -EFAULT;

	if (rsv.flags & ~PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;
	return ops->pr_reserve(bdev, rsv.key, rsv.type, rsv.flags);
}

static int blkdev_pr_release(struct block_device *bdev, blk_mode_t mode,
		struct pr_reservation __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops;
	struct pr_reservation rsv;

	if (!blkdev_pr_allowed(bdev, mode))
		return -EPERM;
	if (!ops || !ops->pr_release)
		return -EOPNOTSUPP;
	if (copy_from_user(&rsv, arg, sizeof(rsv)))
		return -EFAULT;

	if (rsv.flags)
		return -EOPNOTSUPP;
	return ops->pr_release(bdev, rsv.key, rsv.type);
}

static int blkdev_pr_preempt(struct block_device *bdev, blk_mode_t mode,
		struct pr_preempt __user *arg, bool abort)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops;
	struct pr_preempt p;

	if (!blkdev_pr_allowed(bdev, mode))
		return -EPERM;
	if (!ops || !ops->pr_preempt)
		return -EOPNOTSUPP;
	if (copy_from_user(&p, arg, sizeof(p)))
		return -EFAULT;

	if (p.flags)
		return -EOPNOTSUPP;
	return ops->pr_preempt(bdev, p.old_key, p.new_key, p.type, abort);
}

static int blkdev_pr_clear(struct block_device *bdev, blk_mode_t mode,
		struct pr_clear __user *arg)
{
	const struct pr_ops *ops = bdev->bd_disk->fops->pr_ops;
	struct pr_clear c;

	if (!blkdev_pr_allowed(bdev, mode))
		return -EPERM;
	if (!ops || !ops->pr_clear)
		return -EOPNOTSUPP;
	if (copy_from_user(&c, arg, sizeof(c)))
		return -EFAULT;

	if (c.flags)
		return -EOPNOTSUPP;
	return ops->pr_clear(bdev, c.key);
}

static int blkdev_flushbuf(struct block_device *bdev, unsigned cmd,
		unsigned long arg)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	mutex_lock(&bdev->bd_holder_lock);
	if (bdev->bd_holder_ops && bdev->bd_holder_ops->sync)
		bdev->bd_holder_ops->sync(bdev);
	else {
		mutex_unlock(&bdev->bd_holder_lock);
		sync_blockdev(bdev);
	}

	invalidate_bdev(bdev);
	return 0;
}

static int blkdev_roset(struct block_device *bdev, unsigned cmd,
		unsigned long arg)
{
	int ret, n;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (get_user(n, (int __user *)arg))
		return -EFAULT;
	if (bdev->bd_disk->fops->set_read_only) {
		ret = bdev->bd_disk->fops->set_read_only(bdev, n);
		if (ret)
			return ret;
	}
	if (n)
		bdev_set_flag(bdev, BD_READ_ONLY);
	else
		bdev_clear_flag(bdev, BD_READ_ONLY);
	return 0;
}

static int blkdev_getgeo(struct block_device *bdev,
		struct hd_geometry __user *argp)
{
	struct gendisk *disk = bdev->bd_disk;
	struct hd_geometry geo;
	int ret;

	if (!argp)
		return -EINVAL;
	if (!disk->fops->getgeo)
		return -ENOTTY;

	/*
	 * We need to set the startsect first, the driver may
	 * want to override it.
	 */
	memset(&geo, 0, sizeof(geo));
	geo.start = get_start_sect(bdev);
	ret = disk->fops->getgeo(bdev, &geo);
	if (ret)
		return ret;
	if (copy_to_user(argp, &geo, sizeof(geo)))
		return -EFAULT;
	return 0;
}

#ifdef CONFIG_COMPAT
struct compat_hd_geometry {
	unsigned char heads;
	unsigned char sectors;
	unsigned short cylinders;
	u32 start;
};

static int compat_hdio_getgeo(struct block_device *bdev,
			      struct compat_hd_geometry __user *ugeo)
{
	struct gendisk *disk = bdev->bd_disk;
	struct hd_geometry geo;
	int ret;

	if (!ugeo)
		return -EINVAL;
	if (!disk->fops->getgeo)
		return -ENOTTY;

	memset(&geo, 0, sizeof(geo));
	/*
	 * We need to set the startsect first, the driver may
	 * want to override it.
	 */
	geo.start = get_start_sect(bdev);
	ret = disk->fops->getgeo(bdev, &geo);
	if (ret)
		return ret;

	ret = copy_to_user(ugeo, &geo, 4);
	ret |= put_user(geo.start, &ugeo->start);
	if (ret)
		ret = -EFAULT;

	return ret;
}
#endif

/* set the logical block size */
static int blkdev_bszset(struct file *file, blk_mode_t mode,
		int __user *argp)
{
	// this one might be file_inode(file)->i_rdev - a rare valid
	// use of file_inode() for those.
	dev_t dev = I_BDEV(file->f_mapping->host)->bd_dev;
	struct file *excl_file;
	int ret, n;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (!argp)
		return -EINVAL;
	if (get_user(n, argp))
		return -EFAULT;

	if (mode & BLK_OPEN_EXCL)
		return set_blocksize(file, n);

	excl_file = bdev_file_open_by_dev(dev, mode, &dev, NULL);
	if (IS_ERR(excl_file))
		return -EBUSY;
	ret = set_blocksize(excl_file, n);
	fput(excl_file);
	return ret;
}

/*
 * Common commands that are handled the same way on native and compat
 * user space. Note the separate arg/argp parameters that are needed
 * to deal with the compat_ptr() conversion.
 */
static int blkdev_common_ioctl(struct block_device *bdev, blk_mode_t mode,
			       unsigned int cmd, unsigned long arg,
			       void __user *argp)
{
	unsigned int max_sectors;

	switch (cmd) {
	case BLKFLSBUF:
		return blkdev_flushbuf(bdev, cmd, arg);
	case BLKROSET:
		return blkdev_roset(bdev, cmd, arg);
	case BLKDISCARD:
		return blk_ioctl_discard(bdev, mode, arg);
	case BLKSECDISCARD:
		return blk_ioctl_secure_erase(bdev, mode, argp);
	case BLKZEROOUT:
		return blk_ioctl_zeroout(bdev, mode, arg);
	case BLKGETDISKSEQ:
		return put_u64(argp, bdev->bd_disk->diskseq);
	case BLKREPORTZONE:
		return blkdev_report_zones_ioctl(bdev, cmd, arg);
	case BLKRESETZONE:
	case BLKOPENZONE:
	case BLKCLOSEZONE:
	case BLKFINISHZONE:
		return blkdev_zone_mgmt_ioctl(bdev, mode, cmd, arg);
	case BLKGETZONESZ:
		return put_uint(argp, bdev_zone_sectors(bdev));
	case BLKGETNRZONES:
		return put_uint(argp, bdev_nr_zones(bdev));
	case BLKROGET:
		return put_int(argp, bdev_read_only(bdev) != 0);
	case BLKSSZGET: /* get block device logical block size */
		return put_int(argp, bdev_logical_block_size(bdev));
	case BLKPBSZGET: /* get block device physical block size */
		return put_uint(argp, bdev_physical_block_size(bdev));
	case BLKIOMIN:
		return put_uint(argp, bdev_io_min(bdev));
	case BLKIOOPT:
		return put_uint(argp, bdev_io_opt(bdev));
	case BLKALIGNOFF:
		return put_int(argp, bdev_alignment_offset(bdev));
	case BLKDISCARDZEROES:
		return put_uint(argp, 0);
	case BLKSECTGET:
		max_sectors = min_t(unsigned int, USHRT_MAX,
				    queue_max_sectors(bdev_get_queue(bdev)));
		return put_ushort(argp, max_sectors);
	case BLKROTATIONAL:
		return put_ushort(argp, !bdev_nonrot(bdev));
	case BLKRASET:
	case BLKFRASET:
		if(!capable(CAP_SYS_ADMIN))
			return -EACCES;
		bdev->bd_disk->bdi->ra_pages = (arg * 512) / PAGE_SIZE;
		return 0;
	case BLKRRPART:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (bdev_is_partition(bdev))
			return -EINVAL;
		return disk_scan_partitions(bdev->bd_disk,
				mode | BLK_OPEN_STRICT_SCAN);
	case BLKTRACESTART:
	case BLKTRACESTOP:
	case BLKTRACETEARDOWN:
		return blk_trace_ioctl(bdev, cmd, argp);
	case BLKCRYPTOIMPORTKEY:
	case BLKCRYPTOGENERATEKEY:
	case BLKCRYPTOPREPAREKEY:
		return blk_crypto_ioctl(bdev, cmd, argp);
	case IOC_PR_REGISTER:
		return blkdev_pr_register(bdev, mode, argp);
	case IOC_PR_RESERVE:
		return blkdev_pr_reserve(bdev, mode, argp);
	case IOC_PR_RELEASE:
		return blkdev_pr_release(bdev, mode, argp);
	case IOC_PR_PREEMPT:
		return blkdev_pr_preempt(bdev, mode, argp, false);
	case IOC_PR_PREEMPT_ABORT:
		return blkdev_pr_preempt(bdev, mode, argp, true);
	case IOC_PR_CLEAR:
		return blkdev_pr_clear(bdev, mode, argp);
	default:
		return blk_get_meta_cap(bdev, cmd, argp);
	}
}

/*
 * Always keep this in sync with compat_blkdev_ioctl()
 * to handle all incompatible commands in both functions.
 *
 * New commands must be compatible and go into blkdev_common_ioctl
 */
long blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	struct block_device *bdev = I_BDEV(file->f_mapping->host);
	void __user *argp = (void __user *)arg;
	blk_mode_t mode = file_to_blk_mode(file);
	int ret;

	switch (cmd) {
	/* These need separate implementations for the data structure */
	case HDIO_GETGEO:
		return blkdev_getgeo(bdev, argp);
	case BLKPG:
		return blkpg_ioctl(bdev, argp);

	/* Compat mode returns 32-bit data instead of 'long' */
	case BLKRAGET:
	case BLKFRAGET:
		if (!argp)
			return -EINVAL;
		return put_long(argp,
			(bdev->bd_disk->bdi->ra_pages * PAGE_SIZE) / 512);
	case BLKGETSIZE:
		if (bdev_nr_sectors(bdev) > ~0UL)
			return -EFBIG;
		return put_ulong(argp, bdev_nr_sectors(bdev));

	/* The data is compatible, but the command number is different */
	case BLKBSZGET: /* get block device soft block size (cf. BLKSSZGET) */
		return put_int(argp, block_size(bdev));
	case BLKBSZSET:
		return blkdev_bszset(file, mode, argp);
	case BLKGETSIZE64:
		return put_u64(argp, bdev_nr_bytes(bdev));

	/* Incompatible alignment on i386 */
	case BLKTRACESETUP:
		return blk_trace_ioctl(bdev, cmd, argp);
	default:
		break;
	}

	ret = blkdev_common_ioctl(bdev, mode, cmd, arg, argp);
	if (ret != -ENOIOCTLCMD)
		return ret;

	if (!bdev->bd_disk->fops->ioctl)
		return -ENOTTY;
	return bdev->bd_disk->fops->ioctl(bdev, mode, cmd, arg);
}

#ifdef CONFIG_COMPAT

#define BLKBSZGET_32		_IOR(0x12, 112, int)
#define BLKBSZSET_32		_IOW(0x12, 113, int)
#define BLKGETSIZE64_32		_IOR(0x12, 114, int)

/* Most of the generic ioctls are handled in the normal fallback path.
   This assumes the blkdev's low level compat_ioctl always returns
   ENOIOCTLCMD for unknown ioctls. */
long compat_blkdev_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	int ret;
	void __user *argp = compat_ptr(arg);
	struct block_device *bdev = I_BDEV(file->f_mapping->host);
	struct gendisk *disk = bdev->bd_disk;
	blk_mode_t mode = file_to_blk_mode(file);

	switch (cmd) {
	/* These need separate implementations for the data structure */
	case HDIO_GETGEO:
		return compat_hdio_getgeo(bdev, argp);
	case BLKPG:
		return compat_blkpg_ioctl(bdev, argp);

	/* Compat mode returns 32-bit data instead of 'long' */
	case BLKRAGET:
	case BLKFRAGET:
		if (!argp)
			return -EINVAL;
		return compat_put_long(argp,
			(bdev->bd_disk->bdi->ra_pages * PAGE_SIZE) / 512);
	case BLKGETSIZE:
		if (bdev_nr_sectors(bdev) > ~(compat_ulong_t)0)
			return -EFBIG;
		return compat_put_ulong(argp, bdev_nr_sectors(bdev));

	/* The data is compatible, but the command number is different */
	case BLKBSZGET_32: /* get the logical block size (cf. BLKSSZGET) */
		return put_int(argp, bdev_logical_block_size(bdev));
	case BLKBSZSET_32:
		return blkdev_bszset(file, mode, argp);
	case BLKGETSIZE64_32:
		return put_u64(argp, bdev_nr_bytes(bdev));

	/* Incompatible alignment on i386 */
	case BLKTRACESETUP32:
		return blk_trace_ioctl(bdev, cmd, argp);
	default:
		break;
	}

	ret = blkdev_common_ioctl(bdev, mode, cmd, arg, argp);
	if (ret == -ENOIOCTLCMD && disk->fops->compat_ioctl)
		ret = disk->fops->compat_ioctl(bdev, mode, cmd, arg);

	return ret;
}
#endif

struct blk_iou_cmd {
	int res;
	bool nowait;
};

static void blk_cmd_complete(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd);

	if (bic->res == -EAGAIN && bic->nowait)
		io_uring_cmd_issue_blocking(cmd);
	else
		io_uring_cmd_done(cmd, bic->res, 0, issue_flags);
}

static void bio_cmd_bio_end_io(struct bio *bio)
{
	struct io_uring_cmd *cmd = bio->bi_private;
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd);

	if (unlikely(bio->bi_status) && !bic->res)
		bic->res = blk_status_to_errno(bio->bi_status);

	io_uring_cmd_do_in_task_lazy(cmd, blk_cmd_complete);
	bio_put(bio);
}

static int blkdev_cmd_discard(struct io_uring_cmd *cmd,
			      struct block_device *bdev,
			      uint64_t start, uint64_t len, bool nowait)
{
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd);
	gfp_t gfp = nowait ? GFP_NOWAIT : GFP_KERNEL;
	sector_t sector = start >> SECTOR_SHIFT;
	sector_t nr_sects = len >> SECTOR_SHIFT;
	struct bio *prev = NULL, *bio;
	int err;

	if (!bdev_max_discard_sectors(bdev))
		return -EOPNOTSUPP;
	if (!(file_to_blk_mode(cmd->file) & BLK_OPEN_WRITE))
		return -EBADF;
	if (bdev_read_only(bdev))
		return -EPERM;
	err = blk_validate_byte_range(bdev, start, len);
	if (err)
		return err;

	err = filemap_invalidate_pages(bdev->bd_mapping, start,
					start + len - 1, nowait);
	if (err)
		return err;

	while (true) {
		bio = blk_alloc_discard_bio(bdev, &sector, &nr_sects, gfp);
		if (!bio)
			break;
		if (nowait) {
			/*
			 * Don't allow multi-bio non-blocking submissions as
			 * subsequent bios may fail but we won't get a direct
			 * indication of that. Normally, the caller should
			 * retry from a blocking context.
			 */
			if (unlikely(nr_sects)) {
				bio_put(bio);
				return -EAGAIN;
			}
			bio->bi_opf |= REQ_NOWAIT;
		}

		prev = bio_chain_and_submit(prev, bio);
	}
	if (unlikely(!prev))
		return -EAGAIN;
	if (unlikely(nr_sects))
		bic->res = -EAGAIN;

	prev->bi_private = cmd;
	prev->bi_end_io = bio_cmd_bio_end_io;
	submit_bio(prev);
	return -EIOCBQUEUED;
}

int blkdev_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	struct block_device *bdev = I_BDEV(cmd->file->f_mapping->host);
	struct blk_iou_cmd *bic = io_uring_cmd_to_pdu(cmd, struct blk_iou_cmd);
	const struct io_uring_sqe *sqe = cmd->sqe;
	u32 cmd_op = cmd->cmd_op;
	uint64_t start, len;

	if (unlikely(sqe->ioprio || sqe->__pad1 || sqe->len ||
		     sqe->rw_flags || sqe->file_index))
		return -EINVAL;

	bic->res = 0;
	bic->nowait = issue_flags & IO_URING_F_NONBLOCK;

	start = READ_ONCE(sqe->addr);
	len = READ_ONCE(sqe->addr3);

	switch (cmd_op) {
	case BLOCK_URING_CMD_DISCARD:
		return blkdev_cmd_discard(cmd, bdev, start, len, bic->nowait);
	}
	return -EINVAL;
}
