// SPDX-License-Identifier: GPL-2.0
/*
 * BPF struct_ops for FUSE DAX interleaved (striped) extent resolution.
 *
 * Interleaved extents stripe data in round-robin fashion across strips,
 * where each strip is a contiguous allocation on a dax device. Within
 * a single interleaved extent, resolution is O(1) via chunk/strip/stripe
 * arithmetic.
 *
 * GET_FMAP blob format:
 *   struct dax_ileave_wire_hdr
 *   n_iexts * { struct dax_ileave_wire_iext, nstrips * struct dax_ileave_wire_strip }
 *
 * meta_buf format:
 *   struct dax_ileave_meta_hdr
 *   n_iexts * struct dax_ileave_meta_iext
 *   total_strips * struct dax_ileave_meta_strip
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

#define MAX_IEXTS		16
#define MAX_STRIPS_PER_IEXT	32
#define MAX_TOTAL_STRIPS	512

/* GET_FMAP blob wire format */
struct dax_ileave_wire_hdr {
	__u64 file_size;
	__u32 n_iexts;
	__u32 reserved;
};

struct dax_ileave_wire_iext {
	__u64 chunk_size;
	__u32 nstrips;
	__u32 reserved;
	__u64 nbytes;
};

struct dax_ileave_wire_strip {
	__u32 dev_index;
	__u32 reserved;
	__u64 offset;
	__u64 len;
};

/* meta_buf format */
struct dax_ileave_meta_hdr {
	__u32 n_iexts;
	__u32 reserved;
};

struct dax_ileave_meta_iext {
	__u64 chunk_size;
	__u64 nstrips;
	__u64 nbytes;
	__u32 strip_base;
	__u32 reserved;
};

struct dax_ileave_meta_strip {
	__u64 dev_index;
	__u64 offset;
	__u64 len;
};

SEC("struct_ops.s/dax_fmap_parse")
int BPF_PROG(dax_ileave_parse, struct fuse_dax_fmap_parse_ctx *pctx)
{
	const struct dax_ileave_wire_hdr *whdr;
	struct dax_ileave_meta_hdr *mhdr;
	__u32 n_iexts;
	__u32 blob_off, strip_idx = 0;
	__u32 iexts_meta_off;

	whdr = (const struct dax_ileave_wire_hdr *)
		bpf_fuse_dax_parse_get_blob(pctx, 0, sizeof(*whdr));
	if (!whdr)
		return -EINVAL;

	n_iexts = whdr->n_iexts;

	if (n_iexts > MAX_IEXTS)
		return -EINVAL;

	mhdr = (struct dax_ileave_meta_hdr *)
		bpf_fuse_dax_parse_get_meta(pctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EINVAL;

	mhdr->n_iexts = n_iexts;
	mhdr->reserved = 0;

	blob_off = sizeof(*whdr);

	/* Strips array starts after iext headers in meta_buf */
	iexts_meta_off = sizeof(*mhdr);

	/* Parse interleaved extents and their strips */
	for (__u32 i = 0; i < MAX_IEXTS && i < n_iexts; i++) {
		const struct dax_ileave_wire_iext *wiext;
		struct dax_ileave_meta_iext *miext;
		__u32 nstrips;
		__u32 meta_off = iexts_meta_off + i * sizeof(*miext);

		wiext = (const struct dax_ileave_wire_iext *)
			bpf_fuse_dax_parse_get_blob(pctx, blob_off,
						    sizeof(*wiext));
		if (!wiext)
			return -EINVAL;

		nstrips = wiext->nstrips;
		if (nstrips > MAX_STRIPS_PER_IEXT || nstrips == 0)
			return -EINVAL;
		if (wiext->chunk_size == 0)
			return -EINVAL;

		miext = (struct dax_ileave_meta_iext *)
			bpf_fuse_dax_parse_get_meta(pctx, meta_off,
						    sizeof(*miext));
		if (!miext)
			return -EINVAL;

		miext->chunk_size = wiext->chunk_size;
		miext->nstrips = nstrips;
		miext->nbytes = wiext->nbytes;
		miext->strip_base = strip_idx;
		miext->reserved = 0;

		blob_off += sizeof(*wiext);

		/* Parse strips for this iext */
		for (__u32 j = 0; j < MAX_STRIPS_PER_IEXT && j < nstrips; j++) {
			const struct dax_ileave_wire_strip *wstrip;
			struct dax_ileave_meta_strip *mstrip;
			__u32 strips_start = iexts_meta_off +
				n_iexts * sizeof(*miext);
			__u32 soff = strips_start +
				(strip_idx + j) * sizeof(*mstrip);

			if (strip_idx + j >= MAX_TOTAL_STRIPS)
				return -EINVAL;

			wstrip = (const struct dax_ileave_wire_strip *)
				bpf_fuse_dax_parse_get_blob(pctx, blob_off,
							    sizeof(*wstrip));
			if (!wstrip)
				return -EINVAL;

			mstrip = (struct dax_ileave_meta_strip *)
				bpf_fuse_dax_parse_get_meta(pctx, soff,
							    sizeof(*mstrip));
			if (!mstrip)
				return -EINVAL;

			mstrip->dev_index = wstrip->dev_index;
			mstrip->offset = wstrip->offset;
			mstrip->len = wstrip->len;

			pctx->dev_bitmap |= (1ULL << wstrip->dev_index);

			blob_off += sizeof(*wstrip);
		}

		strip_idx += nstrips;
	}

	pctx->file_size = whdr->file_size;

	return 0;
}

SEC("struct_ops/iomap_begin")
int BPF_PROG(dax_ileave_iomap_begin, struct fuse_dax_fmap_resolve_ctx *rctx,
	     struct fuse_iomap_io *io)
{
	const struct dax_ileave_meta_hdr *mhdr;
	__u64 local_offset = rctx->file_offset;
	__u32 n_iexts;

	mhdr = (const struct dax_ileave_meta_hdr *)
		bpf_fuse_dax_resolve_get_meta(rctx, 0, sizeof(*mhdr));
	if (!mhdr)
		return -EIO;

	n_iexts = mhdr->n_iexts;
	if (n_iexts > MAX_IEXTS)
		return -EIO;

	for (__u32 i = 0; i < MAX_IEXTS && i < n_iexts; i++) {
		const struct dax_ileave_meta_iext *miext;
		__u32 iext_off = sizeof(*mhdr) + i * sizeof(*miext);
		__u64 chunk_size, nstrips, ext_size;

		miext = (const struct dax_ileave_meta_iext *)
			bpf_fuse_dax_resolve_get_meta(rctx, iext_off,
						      sizeof(*miext));
		if (!miext)
			return -EIO;

		chunk_size = miext->chunk_size;
		nstrips = miext->nstrips;
		ext_size = miext->nbytes;

		if (chunk_size == 0 || nstrips == 0)
			return -EIO;

		if (ext_size > rctx->file_size)
			ext_size = rctx->file_size;

		if (local_offset < ext_size) {
			const struct dax_ileave_meta_strip *mstrip;
			__u64 chunk_num = local_offset / chunk_size;
			__u64 chunk_offset = local_offset % chunk_size;
			__u64 chunk_remainder = chunk_size - chunk_offset;
			__u64 strip_num = chunk_num % nstrips;
			__u64 stripe_num = chunk_num / nstrips;
			__u64 strip_offset = chunk_offset +
				stripe_num * chunk_size;
			__u32 strip_idx = miext->strip_base + (__u32)strip_num;
			__u32 strips_start = sizeof(*mhdr) +
				n_iexts * sizeof(*miext);
			__u32 soff = strips_start +
				strip_idx * sizeof(*mstrip);

			mstrip = (const struct dax_ileave_meta_strip *)
				bpf_fuse_dax_resolve_get_meta(rctx, soff,
							      sizeof(*mstrip));
			if (!mstrip)
				return -EIO;

			io->dev_index = (__u32)mstrip->dev_index;
			io->addr = mstrip->offset + strip_offset;
			io->length = chunk_remainder < rctx->length
				? chunk_remainder : rctx->length;
			io->offset = rctx->file_offset;
			io->type = 0;
			io->flags = 0;
			return 0;
		}

		local_offset -= ext_size;
	}

	return -EIO;
}

SEC(".struct_ops.link")
struct fuse_dax_fmap_ops dax_ileave_ops = {
	.name = "dax_interleave",
	.dax_fmap_parse = (void *)dax_ileave_parse,
	.iomap_begin = (void *)dax_ileave_iomap_begin,
};
