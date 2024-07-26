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
 * There are two extent formats: simple and interleaved.
 *
 * Simple extents are just (devindex, offset, length) tuples, where devindex
 * references a devdax device that has been registered with the kernel (via
 * FUSE_DEV_IOC_DAXDEV_OPEN). The extent list must cover at least file_size.
 *
 * Interleaved extents merit some additional explanation. An interleaved
 * extent stripes data across a collection of strips. Each strip is a
 * contiguous allocation from a single devdax device, described by a
 * simple-extent structure.
 *
 * Interleaved-extent example:
 *   ie_nstrips = 4
 *   ie_chunk_size = 2MiB
 *   ie_nbytes = 24MiB
 *
 *   +-----------+-----------+-----------+-----------+
 *   | Chunk 0   | Chunk 1   | Chunk 2   | Chunk 3   |
 *   | Strip 0   | Strip 1   | Strip 2   | Strip 3   |
 *   | Stripe 0  | Stripe 0  | Stripe 0  | Stripe 0  |
 *   +-----------+-----------+-----------+-----------+
 *   | Chunk 4   | Chunk 5   | Chunk 6   | Chunk 7   |
 *   | Strip 0   | Strip 1   | Strip 2   | Strip 3   |
 *   | Stripe 1  | Stripe 1  | Stripe 1  | Stripe 1  |
 *   +-----------+-----------+-----------+-----------+
 *   | Chunk 8   | Chunk 9   | Chunk 10  | Chunk 11  |
 *   | Strip 0   | Strip 1   | Strip 2   | Strip 3   |
 *   | Stripe 2  | Stripe 2  | Stripe 2  | Stripe 2  |
 *   +-----------+-----------+-----------+-----------+
 *
 * * Data is laid out across chunks in chunk # order
 * * Columns are strips
 * * Strips are contiguous devdax extents, normally each coming from a
 *   different memory device
 * * Rows are stripes
 * * The number of chunks is (int)((file_size + chunk_size - 1) / chunk_size)
 *   (and obviously the last chunk could be partial)
 * * The stripe_size = (nstrips * chunk_size)
 * * chunk_num(offset)  = offset / chunk_size     // integer division
 * * strip_num(offset)  = chunk_num(offset) % nstrips
 * * stripe_num(offset) = offset / stripe_size    // integer division
 * * ...You get the idea - see the code for more details...
 *
 * Some concrete examples from the layout above:
 * * Offset 0 in the file is offset 0 in chunk 0, which is offset 0 in
 *   strip 0
 * * Offset 4MiB in the file is offset 0 in chunk 2, which is offset 0 in
 *   strip 2
 * * Offset 15MiB in the file is offset 1MiB in chunk 7, which is offset
 *   3MiB in strip 3
 *
 * Notes about this metadata format:
 *
 * * chunk_size must be exactly one supported allocation unit: PAGE_SIZE
 *   (4KiB) or PMD_SIZE (2MiB).
 * * Since chunk_size and nstrips are constant within an interleaved extent,
 *   resolving a file offset to a strip offset within a single interleaved
 *   extent is order 1.
 * * If nstrips == 1, a list of interleaved extents degenerates to a regular
 *   extent list (albeit with some wasted struct space).
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

enum famfs_extent_type {
	SIMPLE_DAX_EXTENT,
	INTERLEAVED_EXTENT,
};

struct famfs_meta_simple_ext {
	u64 dev_index;
	u64 ext_offset;
	u64 ext_len;
};

struct famfs_meta_interleaved_ext {
	u64 fie_nstrips;
	u64 fie_chunk_size;
	u64 fie_nbytes;
	struct famfs_meta_simple_ext *ie_strips;
};

/*
 * Each famfs dax file has this hanging from its fuse_inode->famfs_meta
 */
struct famfs_file_meta {
	bool                   error;
	enum famfs_file_type   file_type;
	size_t                 file_size;
	enum famfs_extent_type fm_extent_type;
	u64 dev_bitmap; /* bitmap of referenced daxdevs by index */
	union {
		struct { /* fm_extent_type == SIMPLE_DAX_EXTENT */
			size_t         fm_nextents;
			/*
			 * If every extent but the last is the same power-of-2
			 * size, ext_shift is ilog2(that size) and the resolver
			 * indexes se[] by a shift of the file offset (O(1)).
			 * Otherwise ext_shift is 0 and the resolver walks the
			 * list (single-extent or non-uniform files).
			 */
			u32            ext_shift;
			struct famfs_meta_simple_ext  *se;
		};
		struct { /* fm_extent_type == INTERLEAVED_EXTENT */
			size_t         fm_niext;
			struct famfs_meta_interleaved_ext *ie;
		};
	};
};

/*
 * famfs_daxdev - tracking struct for a daxdev within a famfs file system
 *
 * This is the in-memory daxdev metadata that is populated by parsing
 * the responses to GET_FMAP messages
 */
struct famfs_daxdev {
	/* Include dev uuid? */
	bool valid;
	bool error; /* Dax has reported a memory error (probably poison) */
	bool dax_err; /* fs_dax_get() failed */
	dev_t devno;
	struct dax_device *devp;
	char *name;
};

#define MAX_DAXDEVS 64

/*
 * famfs_dax_devlist - list of famfs_daxdev's
 */
struct famfs_dax_devlist {
	int nslots;
	int ndevs;
	struct famfs_daxdev *devlist;
};

#endif /* FAMFS_KFMAP_H */
