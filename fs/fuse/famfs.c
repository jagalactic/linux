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
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/string.h>

#include "fuse_i.h"


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

	/* We retrieved the "fmap" (the file's map to memory), but
	 * we haven't used it yet. A call to famfs_file_init_dax() will be added
	 * here in a subsequent patch, when we add the ability to attach
	 * fmaps to files.
	 */

	kvfree(fmap_buf);
	return 0;
}
