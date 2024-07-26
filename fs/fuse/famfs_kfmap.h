/* SPDX-License-Identifier: GPL-2.0 */
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2026 Micron Technology, Inc.
 */
#ifndef FAMFS_KFMAP_H
#define FAMFS_KFMAP_H

/* KABI version 43 (aka v2) fmap structures
 *
 * The location of the memory backing for a famfs file is described by
 * the response to the GET_FMAP fuse message (defined in
 * include/uapi/linux/fuse.h).
 *
 * A famfs file is described by a list of simple extents: (devindex, offset,
 * length) tuples, where devindex references a devdax device that has been
 * registered with the kernel. The extent list must cover at least file_size.
 * Striping/interleaving is expressed by unrolling into a longer simple-extent
 * list in userspace; the kernel handles only simple extents.
 */

/*
 * The structures below are the in-memory metadata format for famfs files.
 * Metadata retrieved via the GET_FMAP response is converted to this format
 * for use in resolving file mapping faults.
 *
 * The GET_FMAP response contains the same information, but in a more
 * message-and-versioning-friendly format. Those structs can be found in the
 * famfs section of include/uapi/linux/fuse.h (aka fuse_kernel.h in libfuse)
 */

enum famfs_file_type {
	FAMFS_REG,
	FAMFS_SUPERBLOCK,
	FAMFS_LOG,
};

struct famfs_meta_simple_ext {
	u64 dev_index;
	u64 ext_offset;
	u64 ext_len;
};

/*
 * Each famfs dax file has this hanging from its fuse_inode->famfs_meta
 */
struct famfs_file_meta {
	bool                   error;
	enum famfs_file_type   file_type;
	size_t                 file_size;
	u64 dev_bitmap; /* bitmap of referenced daxdevs by index */
	size_t                 fm_nextents;
	/*
	 * If every extent but the last is the same power-of-2 size, ext_shift
	 * is ilog2(that size) and the resolver indexes se[] by a shift of the
	 * file offset (O(1)). Otherwise ext_shift is 0 and the resolver walks
	 * the list (single-extent or non-uniform files).
	 */
	u32                    ext_shift;
	struct famfs_meta_simple_ext  *se;
};

#endif /* FAMFS_KFMAP_H */
