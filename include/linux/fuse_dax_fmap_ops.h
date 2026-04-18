/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUSE_DAX_FMAP_OPS_H
#define _LINUX_FUSE_DAX_FMAP_OPS_H

#include <linux/types.h>

#define FUSE_DAX_FMAP_OPS_NAME_LEN 16
#define FUSE_DAX_FMAP_META_MAX (64 * 1024)
#define FUSE_DAX_FMAP_MAX_DEVS 24

struct fuse_dax_fmap_devinfo {
	__u32 dev_index;
	__u32 reserved;
	char name[256];
};

/*
 * Context passed to the BPF parse() callback at file open time.
 * Process context, sleepable. Called once per file open.
 *
 * The raw xattr blob and meta_buf are NOT directly accessible as
 * pointer fields. BPF programs use kfuncs to get bounded pointers:
 *   bpf_fuse_dax_parse_get_xattr() - read the xattr blob
 *   bpf_fuse_dax_parse_get_meta()  - write the metadata buffer
 *   bpf_fuse_dax_parse_get_devs()  - write the device info array
 *
 * Scalar OUT fields are written directly by the BPF program.
 */
struct fuse_dax_fmap_parse_ctx {
	__u32 xattr_blob_size;
	__u32 meta_buf_size;
	__u64 file_size;		/* OUT: file size from fmap */
	__u64 dev_bitmap;		/* OUT: bitmap of referenced dev indices */
	__u32 n_devices;		/* OUT: number of devices populated */
};

/*
 * Context passed to the BPF resolve() callback at iomap_begin time.
 * Non-sleepable, hot path. Called on every page fault / IO.
 *
 * The meta_buf is accessed via kfunc:
 *   bpf_fuse_dax_resolve_get_meta() - read the metadata buffer
 *
 * Scalar OUT fields are written directly by the BPF program.
 */
struct fuse_dax_fmap_resolve_ctx {
	__u32 meta_buf_size;
	__u64 file_offset;
	__u64 length;
	__u64 file_size;
	__u32 dev_index;		/* OUT: index into daxdev table */
	__u64 phys_offset;		/* OUT: byte offset on the device */
	__u64 mapped_length;		/* OUT: bytes mapped from phys_offset */
};

/*
 * BPF struct_ops for FUSE DAX fmap extent resolution.
 *
 * parse() is called at file open in process context (sleepable).
 * resolve() is called at iomap_begin in fault context (non-sleepable).
 */
struct fuse_dax_fmap_ops {
	char name[FUSE_DAX_FMAP_OPS_NAME_LEN];
	int (*parse)(struct fuse_dax_fmap_parse_ctx *ctx);
	int (*resolve)(struct fuse_dax_fmap_resolve_ctx *ctx);
};

/*
 * Kernel-internal context wrappers. The kern structs embed the
 * user-visible ctx plus the actual buffer pointers. BPF programs
 * see only the user-visible portion; kfuncs bridge to the buffers
 * via container_of().
 */
struct fuse_dax_fmap_parse_ctx_kern {
	struct fuse_dax_fmap_parse_ctx ctx;
	const void *xattr_blob;
	void *meta_buf;
	struct fuse_dax_fmap_devinfo *devices;
};

struct fuse_dax_fmap_resolve_ctx_kern {
	struct fuse_dax_fmap_resolve_ctx ctx;
	const void *meta_buf;
};

#endif /* _LINUX_FUSE_DAX_FMAP_OPS_H */
