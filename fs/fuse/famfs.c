// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2026 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/cleanup.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>
#include <linux/overflow.h>
#include <linux/log2.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/string.h>

#include "famfs_kfmap.h"
#include "fuse_i.h"

static void famfs_set_daxdev_err(
	struct fuse_conn *fc, struct dax_device *dax_devp);

static int
famfs_dax_notify_failure(struct dax_device *dax_devp, u64 offset,
			u64 len, int mf_flags)
{
	struct fuse_conn *fc = dax_holder(dax_devp);

	famfs_set_daxdev_err(fc, dax_devp);

	return 0;
}

static const struct dax_holder_operations famfs_fuse_dax_holder_ops = {
	.notify_failure		= famfs_dax_notify_failure,
};

/*****************************************************************************/

/*
 * famfs_teardown()
 *
 * Deallocate famfs metadata for a fuse_conn
 */
void
famfs_teardown(struct fuse_conn *fc)
{
	struct famfs_dax_devlist *devlist __free(kfree) = NULL;
	int i;

	/*
	 * Detach the table under the same lock famfs_set_daxdev_err() takes, so
	 * a notify_failure racing teardown either runs first against the live
	 * table or observes dax_devlist == NULL and bails, rather than
	 * dereferencing it after we clear it. The daxdev holders are dropped
	 * below, after which no further notify_failure can arrive.
	 */
	scoped_guard(rwsem_write, &fc->famfs_devlist_sem) {
		devlist = fc->dax_devlist;
		fc->dax_devlist = NULL;
	}

	if (!devlist)
		return;

	if (!devlist->devlist)
		return;

	/* Close & release all the daxdevs in our table */
	for (i = 0; i < devlist->nslots; i++) {
		struct famfs_daxdev *dd = &devlist->devlist[i];

		if (!dd->valid)
			continue;

		/* Only call fs_put_dax if fs_dax_get succeeded */
		if (dd->devp) {
			if (!dd->dax_err)
				fs_put_dax(dd->devp, fc);
			put_dax(dd->devp);
		}

		kfree(dd->name);
	}
	kfree(devlist->devlist);
}

/* Allocate the daxdev table on first use (idempotent via cmpxchg) */
static int famfs_devlist_alloc(struct fuse_conn *fc)
{
	struct famfs_dax_devlist *devlist;

	if (fc->dax_devlist)
		return 0;

	devlist = kcalloc(1, sizeof(*devlist), GFP_KERNEL);
	if (!devlist)
		return -ENOMEM;

	devlist->nslots = MAX_DAXDEVS;
	devlist->devlist = kcalloc(MAX_DAXDEVS, sizeof(struct famfs_daxdev),
				   GFP_KERNEL);
	if (!devlist->devlist) {
		kfree(devlist);
		return -ENOMEM;
	}

	/* If another thread allocated it first, drop ours */
	if (cmpxchg(&fc->dax_devlist, NULL, devlist) != NULL) {
		kfree(devlist->devlist);
		kfree(devlist);
	}

	return 0;
}

/*
 * famfs_install_daxdev() - exclusively acquire a resolved daxdev and publish
 * it in the table at @index. Shared by the GET_DAXDEV (pull) and
 * DAXDEV_OPEN (push) registration paths.
 *
 * Serializes with concurrent installers under famfs_devlist_sem and rechecks
 * ->valid. A daxdev is entered in the table only once it has been exclusively
 * acquired via fs_dax_get(); on failure the dax_dev_find() reference is
 * released and the slot is left invalid, so the referencing fmap is rejected
 * rather than mapped without an exclusive holder. @name may be NULL (the push
 * path passes no pathname).
 */
static int famfs_install_daxdev(struct fuse_conn *fc, u64 index, dev_t devno,
				const char *name)
{
	struct famfs_daxdev *daxdev;
	int rc = 0;

	if (index >= fc->dax_devlist->nslots) {
		pr_err("%s: index(%llu) >= nslots(%d)\n",
		       __func__, index, fc->dax_devlist->nslots);
		return -EINVAL;
	}

	scoped_guard(rwsem_write, &fc->famfs_devlist_sem) {
		daxdev = &fc->dax_devlist->devlist[index];

		/* Installed already by a concurrent push/pull */
		if (daxdev->valid)
			return 0;

		/*
		 * A prior attempt already determined this daxdev cannot be
		 * exclusively acquired (see the fs_dax_get() failure handling
		 * below). Don't thrash on GET_DAXDEV/fs_dax_get(); fail fast.
		 */
		if (daxdev->dax_err)
			return -EIO;

		daxdev->devp = dax_dev_find(devno);
		if (!daxdev->devp) {
			pr_warn("%s: device %u:%u not found or not dax\n",
				__func__, MAJOR(devno), MINOR(devno));
			return -ENODEV;
		}

		rc = fs_dax_get(daxdev->devp, fc, &famfs_fuse_dax_holder_ops);
		if (rc) {
			/*
			 * Distinguish a lost race from a real failure. -EBUSY
			 * with the daxdev already held by *this* fuse_conn
			 * means a concurrent acquire won and will publish the
			 * slot valid: not an error, and must not be cached as
			 * dax_err. Any other failure (foreign holder, not a dax
			 * device, wrong driver type) is permanent for this
			 * connection, so record dax_err to stop re-fetching and
			 * re-acquiring it.
			 */
			if (!(rc == -EBUSY && dax_holder(daxdev->devp) == fc)) {
				pr_err("%s: fs_dax_get(%u:%u) failed rc=%d\n",
				       __func__, MAJOR(devno), MINOR(devno), rc);
				daxdev->dax_err = true;
			}
			put_dax(daxdev->devp);
			daxdev->devp = NULL;
			return rc;
		}

		daxdev->devno = devno;
		if (name) {
			daxdev->name = kstrdup(name, GFP_KERNEL);
			if (!daxdev->name) {
				fs_put_dax(daxdev->devp, fc);
				put_dax(daxdev->devp);
				daxdev->devp = NULL;
				return -ENOMEM;
			}
		}

		wmb(); /* All other fields must be visible before valid */
		daxdev->valid = 1;
	}

	return 0;
}

/**
 * famfs_daxdev_open() - Register a daxdev via FUSE_DEV_IOC_DAXDEV_OPEN
 * @fc:   fuse_conn
 * @map:  fuse_backing_map; @map->fd is an fd to the devdax device and
 *        @map->daxdev_index is the (cluster-invariant) famfs index.
 *
 * The server pushes a daxdev to the kernel by reference (an fd), rather than
 * the kernel pulling it by name via GET_DAXDEV. The resolved daxdev is
 * exclusively acquired and entered in the table at @map->daxdev_index.
 *
 * Return: 0=success
 *         -errno=failure
 */
int famfs_daxdev_open(struct fuse_conn *fc, struct fuse_backing_map *map)
{
	struct inode *inode;
	struct file *file;
	dev_t devno;
	int rc;

	/* Only fs-dax (famfs) mode accepts daxdev registration */
	if (!fc->famfs_iomap)
		return -EOPNOTSUPP;

	file = fget(map->fd);
	if (!file)
		return -EBADF;

	inode = file_inode(file);
	if (!S_ISCHR(inode->i_mode)) {
		fput(file);
		return -EINVAL;
	}
	devno = inode->i_rdev;
	fput(file);

	rc = famfs_devlist_alloc(fc);
	if (rc)
		return rc;

	rc = famfs_install_daxdev(fc, map->daxdev_index, devno, NULL);
	if (rc)
		pr_err("%s: failed to install daxdev\n", __func__);

	return rc;
}

/**
 * famfs_check_daxdev_table() - Verify an fmap's referenced daxdevs are installed
 * @fm:   fuse_mount
 * @meta: famfs_file_meta, in-memory format, built from a GET_FMAP response
 *
 * Called for each new file fmap. Every daxdev the fmap references must already
 * be installed in the table, having been pushed in via FUSE_DEV_IOC_DAXDEV_OPEN
 * before any file that uses it is accessed. If any referenced daxdev is not
 * present, the fmap is rejected so the file is never mapped against a daxdev
 * that has no exclusive holder.
 *
 * Return: 0=success (all referenced daxdevs present)
 *         <0=a referenced daxdev is missing from the table
 */
static int
famfs_check_daxdev_table(
	struct fuse_mount *fm,
	const struct famfs_file_meta *meta)
{
	struct fuse_conn *fc = fm->fc;
	int nmissing = 0;
	int err;

	err = famfs_devlist_alloc(fc);
	if (err)
		return err;

	/* Count missing daxdevs while holding the reader lock */
	scoped_guard(rwsem_read, &fc->famfs_devlist_sem) {
		unsigned long i;

		for_each_set_bit(i, (unsigned long *)&meta->dev_bitmap,
				 MAX_DAXDEVS) {
			struct famfs_daxdev *dd = &fc->dax_devlist->devlist[i];

			/*
			 * Skip daxdevs already installed (valid) or already
			 * known to be unusable (dax_err). Re-fetching either
			 * just thrashes on GET_DAXDEV and fs_dax_get().
			 */
			if (!dd->valid && !dd->dax_err)
				nmissing++;
		}
	}

	if (nmissing > 0) {
		/* this file referenced at least one daxdev that is not in
		 * the table. Daxdevs must be known before any file that
		 * uses them is accessed
		 */
		pr_err("%s: %d missing daxdev(s)\n", __func__, nmissing);
		return -ENODEV;
	}

	return 0;
}

static void
famfs_set_daxdev_err(
	struct fuse_conn *fc,
	struct dax_device *dax_devp)
{
	int i;

	/*
	 * Search the list by dax_devp under the write lock: we set dd->error,
	 * and it serializes against famfs_teardown() clearing the table.
	 */
	scoped_guard(rwsem_write, &fc->famfs_devlist_sem) {
		if (!fc->dax_devlist)
			return;
		for (i = 0; i < fc->dax_devlist->nslots; i++) {
			if (fc->dax_devlist->devlist[i].valid) {
				struct famfs_daxdev *dd;

				dd = &fc->dax_devlist->devlist[i];
				if (dd->devp != dax_devp)
					continue;

				dd->error = true;

				pr_err("%s: memory error on daxdev %s (%d)\n",
				       __func__, dd->name, i);
				return;
			}
		}
	}
	pr_err("%s: memory err on unrecognized daxdev\n", __func__);
}

/***************************************************************************/

void __famfs_meta_free(void *famfs_meta)
{
	struct famfs_file_meta *fmap = famfs_meta;

	if (!fmap)
		return;

	switch (fmap->fm_extent_type) {
	case SIMPLE_DAX_EXTENT:
		kfree(fmap->se);
		break;
	case INTERLEAVED_EXTENT:
		if (fmap->ie) {
			int i;

			for (i = 0; i < fmap->fm_niext; i++)
				kfree(fmap->ie[i].ie_strips);
		}
		kfree(fmap->ie);
		break;
	}
	kfree(fmap);
}
DEFINE_FREE(__famfs_meta_free, void *, if (_T) __famfs_meta_free(_T))

static int
famfs_check_ext_alignment(struct famfs_meta_simple_ext *se)
{
	int errs = 0;

	if (se->dev_index >= MAX_DAXDEVS)
		errs++;

	/*
	 * Page alignment is the superset: a 2 MiB-aligned extent still passes
	 * and maps with huge pages, while a 4 KiB-granular extent maps with
	 * PTEs (the dax fault path falls back PMD -> PTE).
	 */
	if (!IS_ALIGNED(se->ext_offset, PAGE_SIZE))
		errs++;

	if (!IS_ALIGNED(se->ext_len, PAGE_SIZE))
		errs++;

	return errs;
}

/**
 * famfs_fuse_meta_alloc() - Allocate famfs file metadata
 * @fmap_buf:  fmap buffer from fuse server
 * @fmap_buf_size: size of fmap buffer
 * @metap:         pointer where 'struct famfs_file_meta' is returned
 *
 * Returns: 0=success
 *          -errno=failure
 */
static int
famfs_fuse_meta_alloc(
	void *fmap_buf,
	size_t fmap_buf_size,
	struct famfs_file_meta **metap)
{
	struct fuse_famfs_fmap_header *fmh;
	size_t extent_total = 0;
	size_t next_offset = 0;
	int errs = 0;
	int i, j;
	u64 ext_end;

	fmh = fmap_buf;

	/* Move past fmh in fmap_buf */
	next_offset += sizeof(*fmh);
	if (next_offset > fmap_buf_size) {
		pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
		       __func__, __LINE__, next_offset, fmap_buf_size);
		return -EINVAL;
	}

	if (fmh->nextents < 1) {
		pr_err("%s: nextents %d < 1\n", __func__, fmh->nextents);
		return -ERANGE;
	}

	if (fmh->file_size > MAX_LFS_FILESIZE) {
		pr_err("%s: file_size %lld > MAX_LFS_FILESIZE\n",
		       __func__, fmh->file_size);
		return -EINVAL;
	}

	/*
	 * No separate upper cap on nextents: the reply buffer bounds it. The
	 * extent list is rejected below if it does not fit in fmap_buf_size, and
	 * fuse_get_fmap() already refused to kvmalloc a buffer larger than
	 * FMAP_BUFSIZE_MAX -- so anything that fits is small enough to handle.
	 */

	struct famfs_file_meta *meta __free(__famfs_meta_free) = kzalloc(sizeof(*meta), GFP_KERNEL);

	if (!meta)
		return -ENOMEM;

	meta->error = false;
	meta->file_type = fmh->file_type;
	meta->file_size = fmh->file_size;

	switch (fmh->ext_type) {
	case FUSE_FAMFS_EXT_SIMPLE: {
		struct fuse_famfs_simple_ext *se_in;

		meta->fm_extent_type = SIMPLE_DAX_EXTENT;
		se_in = fmap_buf + next_offset;

		/* Move past simple extents */
		next_offset += fmh->nextents * sizeof(*se_in);
		if (next_offset > fmap_buf_size) {
			pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
			       __func__, __LINE__, next_offset, fmap_buf_size);
			return -EINVAL;
		}

		meta->fm_nextents = fmh->nextents;

		meta->se = kcalloc(meta->fm_nextents, sizeof(*(meta->se)),
				   GFP_KERNEL);
		if (!meta->se)
			return -ENOMEM;

		for (i = 0; i < fmh->nextents; i++) {
			meta->se[i].dev_index  = se_in[i].se_devindex;
			meta->se[i].ext_offset = se_in[i].se_offset;
			meta->se[i].ext_len    = se_in[i].se_len;

			/* Record bitmap of referenced daxdev indices */
			meta->dev_bitmap |= BIT_ULL(meta->se[i].dev_index);

			errs += famfs_check_ext_alignment(&meta->se[i]);

			if (check_add_overflow(meta->se[i].ext_offset,
					       meta->se[i].ext_len, &ext_end) ||
			    check_add_overflow(extent_total,
					       meta->se[i].ext_len,
					       &extent_total))
				return -EINVAL;
		}

		/*
		 * Detect a uniform extent size so the resolver can index se[]
		 * by a shift rather than walking the list. This requires every
		 * extent but the last to be the same power-of-2 size, with the
		 * last no larger. ext_shift stays 0 (walk) for a single extent,
		 * or a non-uniform / non-power-of-2 list.
		 */
		meta->ext_shift = 0;
		if (meta->fm_nextents > 1) {
			u64 esz = meta->se[0].ext_len;
			bool uniform = is_power_of_2(esz);

			for (i = 1; uniform && i < meta->fm_nextents - 1; i++)
				if (meta->se[i].ext_len != esz)
					uniform = false;

			if (uniform && meta->se[meta->fm_nextents - 1].ext_len > esz)
				uniform = false;

			if (uniform)
				meta->ext_shift = ilog2(esz);
		}
		break;
	}

	case FUSE_FAMFS_EXT_INTERLEAVE: {
		u64 logical_total = 0;
		struct fuse_famfs_iext *ie_in;
		int niext = fmh->nextents;

		meta->fm_extent_type = INTERLEAVED_EXTENT;
		meta->fm_niext = niext;

		meta->ie = kcalloc(niext, sizeof(*(meta->ie)), GFP_KERNEL);
		if (!meta->ie)
			return -ENOMEM;

		/*
		 * Each interleaved extent has a strip list of simple extents.
		 * Outer loop is over the separate interleaved extents.
		 */
		for (i = 0; i < niext; i++) {
			struct fuse_famfs_simple_ext *sie_in;
			u64 nstrips, chunks, nprobe, n;

			/* One interleaved-extent header in fmap_buf */
			ie_in = fmap_buf + next_offset;
			next_offset += sizeof(*ie_in);
			if (next_offset > fmap_buf_size) {
				pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
				       __func__, __LINE__, next_offset,
				       fmap_buf_size);
				return -EINVAL;
			}

			/* chunk_size must be exactly one supported alloc unit */
			if (ie_in->ie_chunk_size != PAGE_SIZE &&
			    ie_in->ie_chunk_size != PMD_SIZE) {
				pr_err("%s: bad chunk_size %u\n",
				       __func__, ie_in->ie_chunk_size);
				return -EINVAL;
			}
			if (ie_in->ie_nbytes == 0 ||
			    ie_in->ie_nbytes > MAX_LFS_FILESIZE) {
				pr_err("%s: bad ie_nbytes %llu\n",
				       __func__, ie_in->ie_nbytes);
				return -EINVAL;
			}

			nstrips = ie_in->ie_nstrips;
			if (nstrips < 1 || nstrips > FUSE_FAMFS_MAX_STRIPS) {
				pr_err("%s: invalid nstrips=%llu (max=%d)\n",
				       __func__, nstrips, FUSE_FAMFS_MAX_STRIPS);
				return -EINVAL;
			}

			meta->ie[i].fie_chunk_size = ie_in->ie_chunk_size;
			meta->ie[i].fie_nstrips    = nstrips;
			meta->ie[i].fie_nbytes     = ie_in->ie_nbytes;

			chunks = DIV_ROUND_UP(ie_in->ie_nbytes,
					      ie_in->ie_chunk_size);

			/* The strip extents follow the interleaved-ext header */
			sie_in = fmap_buf + next_offset;
			next_offset += nstrips * sizeof(*sie_in);
			if (next_offset > fmap_buf_size) {
				pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
				       __func__, __LINE__, next_offset,
				       fmap_buf_size);
				return -EINVAL;
			}

			meta->ie[i].ie_strips =
				kcalloc(nstrips, sizeof(meta->ie[i].ie_strips[0]),
					GFP_KERNEL);
			if (!meta->ie[i].ie_strips)
				return -ENOMEM;

			/* Inner loop is over the strips */
			for (j = 0; j < nstrips; j++) {
				struct famfs_meta_simple_ext *so =
					&meta->ie[i].ie_strips[j];

				so->dev_index  = sie_in[j].se_devindex;
				so->ext_offset = sie_in[j].se_offset;
				so->ext_len    = sie_in[j].se_len;

				/* Record bitmap of referenced daxdev indices */
				meta->dev_bitmap |= BIT_ULL(so->dev_index);

				errs += famfs_check_ext_alignment(so);
				if (check_add_overflow(so->ext_offset,
						       so->ext_len, &ext_end) ||
				    check_add_overflow(extent_total,
						       so->ext_len,
						       &extent_total))
					return -EINVAL;
			}

			/*
			 * Validate that every strip physically holds the chunks
			 * striped onto it: probe the highest file offset that
			 * lands on each strip (the last byte of each top-row
			 * chunk) with the same arithmetic the fault path uses. If
			 * those worst-case offsets resolve within their strips,
			 * every lower offset does too. Partial-final-chunk aware;
			 * strips beyond the chunk count map nothing and are
			 * skipped.
			 */
			nprobe = min_t(u64, nstrips, chunks);
			for (n = 0; n < nprobe; n++) {
				u64 c          = chunks - 1 - n;
				u64 stripe_num = c / nstrips;
				u64 strip_num  = c % nstrips;
				u64 chunk_last = (c == chunks - 1) ?
					((ie_in->ie_nbytes - 1) %
					 ie_in->ie_chunk_size) :
					(ie_in->ie_chunk_size - 1);
				u64 strip_off  = stripe_num *
					ie_in->ie_chunk_size + chunk_last;

				if (strip_off >=
				    meta->ie[i].ie_strips[strip_num].ext_len) {
					pr_err("%s: strip %llu too small for its chunks\n",
					       __func__, strip_num);
					return -EINVAL;
				}
			}

			/* This interleaved extent maps ie_nbytes logical bytes */
			if (check_add_overflow(logical_total, ie_in->ie_nbytes,
					       &logical_total))
				return -EINVAL;
		}

		if (logical_total < meta->file_size) {
			pr_err("%s: interleaved extents (0x%llx) do not cover file_size (0x%zx)\n",
			       __func__, logical_total, meta->file_size);
			return -EINVAL;
		}
		break;
	}

	default:
		pr_err("%s: invalid ext_type %d\n", __func__, fmh->ext_type);
		return -EINVAL;
	}

	if (errs > 0) {
		pr_err("%s: %d alignment errors found\n", __func__, errs);
		return -EINVAL;
	}

	/* More sanity checks */
	if (extent_total < meta->file_size) {
		pr_err("%s: file size %ld larger than map size %ld\n",
		       __func__, meta->file_size, extent_total);
		return -EINVAL;
	}

	if (cmpxchg(metap, NULL, meta) != NULL) {
		pr_debug("%s: fmap race detected\n", __func__);
		return 0; /* fmap already installed */
	}
	retain_and_null_ptr(meta);

	return 0;
}

/**
 * famfs_file_init_dax() - init famfs dax file metadata
 *
 * @fm:        fuse_mount
 * @inode:     the inode
 * @fmap_buf:  fmap response message
 * @fmap_size: Size of the fmap message
 *
 * Initialize famfs metadata for a file, based on the contents of the GET_FMAP
 * response
 *
 * Return: 0=success
 *          -errno=failure
 */
int
famfs_file_init_dax(
	struct fuse_mount *fm,
	struct inode *inode,
	void *fmap_buf,
	size_t fmap_size)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct famfs_file_meta *meta = NULL;
	int rc;

	if (fi->famfs_meta) {
		pr_notice("%s: i_no=%llu fmap_size=%zu ALREADY INITIALIZED\n",
			  __func__,
			  inode->i_ino, fmap_size);
		return 0;
	}

	rc = famfs_fuse_meta_alloc(fmap_buf, fmap_size, &meta);
	if (rc)
		goto errout;

	/* Make sure this fmap doesn't reference any unknown daxdevs */
	if (famfs_check_daxdev_table(fm, meta))
		meta->error = true;

	/* Publish the famfs metadata on fi->famfs_meta */
	inode_lock(inode);

	if (famfs_meta_set(fi, meta) == NULL) {
		i_size_write(inode, meta->file_size);
		inode->i_flags |= S_DAX;
	} else {
		pr_debug("%s: file already had metadata\n", __func__);
		__famfs_meta_free(meta);
		/* rc is 0 - the file is valid */
	}

	inode_unlock(inode);
	return 0;

errout:
	if (rc)
		__famfs_meta_free(meta);

	return rc;
}

#define FMAP_BUFSIZE PAGE_SIZE

#define FMAP_BUFSIZE_INIT PAGE_SIZE
/*
 * Largest GET_FMAP reply buffer we will kvmalloc. Any fmap whose whole message
 * fits in this buffer is handled; there is no separate extent-count cap, so the
 * effective extent limit is just this size / sizeof(simple_ext) (~699k extents
 * => ~1.3 TiB per striped file at a 2 MiB chunk). kvmalloc-backed, so it may
 * exceed the contiguous kmalloc limit. Matches the server's reply-buffer cap.
 */
#define FMAP_BUFSIZE_MAX (16 * 1024 * 1024)

int fuse_get_fmap(struct fuse_mount *fm, struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	u64 nodeid = get_node_id(inode);
	size_t bufsize = FMAP_BUFSIZE_INIT;
	void *fmap_buf = NULL;
	ssize_t fmap_size;
	int attempt;
	int rc;

	/* Don't retrieve if we already have the famfs metadata */
	if (fi->famfs_meta)
		return 0;

	/*
	 * The fmap size is not known in advance. Start with a modest buffer and,
	 * if the server reports (via the returned header's fmap_size) that the
	 * whole fmap did not fit, reallocate exactly that size and retry once.
	 * The server learns our buffer size from the request's
	 * fuse_getxattr_in.size (GETXATTR-style size probe).
	 */
	for (attempt = 0; ; attempt++) {
		struct fuse_getxattr_in in = { .size = bufsize };
		struct fuse_famfs_fmap_header *fmh;
		u32 required;

		FUSE_ARGS(args);

		fmap_buf = kvmalloc(bufsize, GFP_KERNEL);
		if (!fmap_buf)
			return -ENOMEM;

		args.opcode = FUSE_GET_FMAP;
		args.nodeid = nodeid;
		args.in_numargs = 1;
		args.in_args[0].size = sizeof(in);
		args.in_args[0].value = &in;
		/*
		 * Variable-sized output buffer; fuse_simple_request() returns
		 * the size of the output payload.
		 */
		args.out_argvar = true;
		args.out_numargs = 1;
		args.out_args[0].size = bufsize;
		args.out_args[0].value = fmap_buf;

		rc = fuse_simple_request(fm, &args);
		if (rc < 0) {
			pr_err("%s: err=%d from fuse_simple_request()\n",
			       __func__, rc);
			kvfree(fmap_buf);
			return rc;
		}
		fmap_size = rc;

		/* Need at least a header to learn the required size */
		if (fmap_size < (ssize_t)sizeof(*fmh)) {
			pr_err("%s: short fmap reply %zd\n", __func__, fmap_size);
			kvfree(fmap_buf);
			return -EIO;
		}

		fmh = fmap_buf;
		required = fmh->fmap_size;

		/* Whole fmap fit in the buffer -> parse it */
		if (required <= bufsize)
			break;

		/* Too small: server sent only the header. Grow and retry once. */
		kvfree(fmap_buf);
		fmap_buf = NULL;

		if (required > FMAP_BUFSIZE_MAX) {
			pr_err("%s: fmap size %u exceeds max %zu\n",
			       __func__, required, (size_t)FMAP_BUFSIZE_MAX);
			return -EFBIG;
		}
		if (attempt >= 1) {
			/*
			 * A famfs file is fixed-size, so the server must report
			 * the same fmap_size on the retry as on the first
			 * request. A larger value means the file's size/fmap
			 * changed between the two GET_FMAPs -- a server bug.
			 */
			pr_err("%s: fmap grew %zu -> %u across GET_FMAP retries; famfs file size must not change (server bug)\n",
			       __func__, bufsize, required);
			return -EINVAL;
		}
		bufsize = required;
	}

	/* Convert fmap into in-memory format and hang from inode */
	rc = famfs_file_init_dax(fm, inode, fmap_buf, fmap_size);

	kvfree(fmap_buf);
	return rc;
}
