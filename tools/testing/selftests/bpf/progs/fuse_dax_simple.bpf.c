// SPDX-License-Identifier: GPL-2.0
/*
 * BPF struct_ops for FUSE DAX simple (linear) extent resolution.
 *
 * GET_FMAP blob format:
 *   struct dax_simple_wire_hdr
 *   n_extents * struct dax_simple_wire_ext
 *
 * meta_buf format:
 *   struct dax_simple_meta_hdr
 *   n_extents * struct dax_simple_meta_ext
 */

#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <linux/errno.h>

char _license[] SEC("license") = "GPL";

/* kfunc declarations */
extern const __u8 *bpf_fuse_dax_parse_get_blob(
	struct fuse_dax_fmap_parse_ctx *ctx,
	__u32 offset, const __u32 rdonly_buf_size) __ksym;
extern __u8 *bpf_fuse_dax_parse_get_meta(
	struct fuse_dax_fmap_parse_ctx *ctx,
	__u32 offset, const __u32 rdwr_buf_size) __ksym;
extern const __u8 *bpf_fuse_dax_resolve_get_meta(
	struct fuse_dax_fmap_resolve_ctx *ctx,
	__u32 offset, const __u32 rdonly_buf_size) __ksym;

#define MAX_SIMPLE_EXTENTS	2048

/* GET_FMAP blob wire format */
struct dax_simple_wire_hdr {
	__u64 file_size;
	__u32 n_extents;
	__u32 reserved;
};

struct dax_simple_wire_ext {
	__u32 dev_index;
	__u32 reserved;
	__u64 offset;
	__u64 len;
};

/* meta_buf format */
struct dax_simple_meta_hdr {
	__u32 n_extents;
	__u32 reserved;
};

struct dax_simple_meta_ext {
	__u32 dev_index;
	__u32 reserved;
	__u64 offset;
	__u64 len;
};

SEC("struct_ops.s/dax_fmap_parse")
int BPF_PROG(dax_simple_parse, struct fuse_dax_fmap_parse_ctx *pctx)
{
	const struct dax_simple_wire_hdr *whdr;
	struct dax_simple_meta_hdr *mhdr;
	__u32 n_extents;
	__u32 blob_off;

	whdr = (const struct dax_simple_wire_hdr *)
		bpf_fuse_dax_parse_get_blob(pctx, 0, sizeof(*whdr));
	if (!whdr)
		return -EINVAL;

	n_extents = whdr->n_extents;

	if (n_extents > MAX_SIMPLE_EXTENTS)
		return -EINVAL;

	mhdr = (struct dax_simple_meta_hdr *)
		bpf_fuse_dax_parse_get_meta(pctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EINVAL;

	mhdr->n_extents = n_extents;
	mhdr->reserved = 0;

	blob_off = sizeof(*whdr);

	for (__u32 i = 0; i < MAX_SIMPLE_EXTENTS && i < n_extents; i++) {
		const struct dax_simple_wire_ext *wext;
		struct dax_simple_meta_ext *mext;
		__u32 meta_off = sizeof(*mhdr) + i * sizeof(*mext);

		wext = (const struct dax_simple_wire_ext *)
			bpf_fuse_dax_parse_get_blob(pctx, blob_off,
						    sizeof(*wext));
		if (!wext)
			return -EINVAL;

		mext = (struct dax_simple_meta_ext *)
			bpf_fuse_dax_parse_get_meta(pctx, meta_off,
						    sizeof(*mext));
		if (!mext)
			return -EINVAL;

		mext->dev_index = wext->dev_index;
		mext->reserved = 0;
		mext->offset = wext->offset;
		mext->len = wext->len;

		pctx->dev_bitmap |= (1ULL << wext->dev_index);

		blob_off += sizeof(*wext);
	}

	pctx->file_size = whdr->file_size;

	return 0;
}

SEC("struct_ops/iomap_begin")
int BPF_PROG(dax_simple_iomap_begin, struct fuse_dax_fmap_resolve_ctx *rctx,
	     struct fuse_iomap_io *io)
{
	const struct dax_simple_meta_hdr *mhdr;
	__u64 local_offset = rctx->file_offset;
	__u32 n_extents;

	mhdr = (const struct dax_simple_meta_hdr *)
		bpf_fuse_dax_resolve_get_meta(rctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EIO;

	n_extents = mhdr->n_extents;
	if (n_extents > MAX_SIMPLE_EXTENTS)
		return -EIO;

	for (__u32 i = 0; i < MAX_SIMPLE_EXTENTS && i < n_extents; i++) {
		const struct dax_simple_meta_ext *mext;
		__u32 meta_off = sizeof(*mhdr) + i * sizeof(*mext);

		mext = (const struct dax_simple_meta_ext *)
			bpf_fuse_dax_resolve_get_meta(rctx, meta_off,
						      sizeof(*mext));
		if (!mext)
			return -EIO;

		if (local_offset < mext->len) {
			__u64 remaining = mext->len - local_offset;

			io->dev_index = mext->dev_index;
			io->addr = mext->offset + local_offset;
			io->length = remaining < rctx->length
				? remaining : rctx->length;
			io->offset = rctx->file_offset;
			io->type = 0; /* IOMAP_MAPPED filled by kernel */
			io->flags = 0;
			return 0;
		}
		local_offset -= mext->len;
	}

	return -EIO;
}

SEC(".struct_ops.link")
struct fuse_dax_fmap_ops dax_simple_ops = {
	.name = "dax_simple",
	.dax_fmap_parse = (void *)dax_simple_parse,
	.iomap_begin = (void *)dax_simple_iomap_begin,
};
