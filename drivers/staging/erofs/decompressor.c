// SPDX-License-Identifier: GPL-2.0
/*
 * linux/drivers/staging/erofs/decompressor.c
 *
 * Copyright (C) 2019 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Gao Xiang <gaoxiang25@huawei.com>
 */
#include "compress.h"
#include <linux/lz4.h>

struct z_erofs_decompressor {
	/*
	 * if destpages have sparsed pages, fill them with bounce pages.
	 * it also check whether destpages indicate continuous physical memory.
	 */
	int (*prepare_destpages)(struct z_erofs_decompress_req *rq,
				 struct list_head *pagepool);
	void *(*map_inplace_data)(struct z_erofs_decompress_req *rq);
	void (*unmap_inplace_data)(struct z_erofs_decompress_req *rq,
				   void *src);
	int (*decompress)(const char *src, char *dst,
			  unsigned int srclen, unsigned int dstlen, bool partial);
	char *name;
};

#define LZ4_MAX_DISTANCE_PAGES	(65536 >> PAGE_SHIFT)

static int lz4_prepare_destpages(struct z_erofs_decompress_req *rq,
				 struct list_head *pagepool)
{
	const unsigned int nr =
		PAGE_ALIGN(rq->pageofs_out + rq->outputsize) >> PAGE_SHIFT;
	struct page *availables[LZ4_MAX_DISTANCE_PAGES] = { NULL };
	unsigned long unused[DIV_ROUND_UP(LZ4_MAX_DISTANCE_PAGES, BITS_PER_LONG)] = { 0 };
	void *kaddr = NULL;
	unsigned int i, j, k;

	for (i = 0; i < nr; ++i) {
		struct page *const page = rq->out[i];

		j = i & (LZ4_MAX_DISTANCE_PAGES - 1);
		if (availables[j])
			__set_bit(j, unused);

		if (page) {
			if (kaddr) {
				if (kaddr + PAGE_SIZE == page_address(page))
					kaddr += PAGE_SIZE;
				else
					kaddr = NULL;
			} else if (!i) {
				kaddr = page_address(page);
			}
			continue;
		}
		kaddr = NULL;

		k = find_first_bit(unused, LZ4_MAX_DISTANCE_PAGES);
		if (k < LZ4_MAX_DISTANCE_PAGES) {
			j = k;
			get_page(availables[j]);
		} else {
			DBG_BUGON(availables[j]);
			availables[j] = erofs_allocpage(pagepool, GFP_KERNEL,
							true, false);
			if (!availables[j])
				return -ENOMEM;
			availables[j]->mapping = Z_EROFS_MAPPING_STAGING;
		}
		rq->out[i] = availables[j];
		__clear_bit(j, unused);
	}
	return kaddr ? 1 : 0;
}

static void *generic_map_inplace_data(struct z_erofs_decompress_req *rq)
{
	/*
	 * if in-place decompression is ongoing, those decompressed
	 * pages should be copied in order to avoid being overlapped.
	 */
	struct page **in = rq->in;	
	unsigned int inputsize = PAGE_SIZE, pageofs_in = 0;
	void *const tmp = erofs_get_pcpubuf(0);
	char *tmpp = tmp;

	while (inputsize) {
		const unsigned int count = PAGE_SIZE - pageofs_in;
		char *const src = kmap_atomic(*in);

		memcpy(tmpp, src + pageofs_in, count);
		kunmap_atomic(src);
		inputsize -= count;
		tmpp += count;
		pageofs_in = 0;
		++in;
	}
	return tmp;
}

static void generic_unmap_inplace_data(struct z_erofs_decompress_req *rq,
				       void *src)
{
	if (rq->overlapped_decoding) {
		kunmap_atomic(src);
		return;
	}
	erofs_put_pcpubuf(src, 0);
}

static int lz4_decompress(const char *in, char *out, unsigned int inlen,
			  unsigned int outlen, bool partial)
{
	int ret = LZ4_decompress_safe_partial(in, out, inlen, outlen, outlen);

	if (ret >= 0)
		return 0;

	/*
	 * LZ4_decompress_safe_partial will return an error code
	 * (< 0) if decompression failed
	 */
	errln("%s, failed to decompress, in[%p, %u] outlen[%p, %u]",
	      __func__, in, inlen, out, outlen);
	WARN_ON(1);
	print_hex_dump(KERN_DEBUG, "[ in]: ", DUMP_PREFIX_OFFSET,
		       16, 1, in, inlen, true);
	print_hex_dump(KERN_DEBUG, "[out]: ", DUMP_PREFIX_OFFSET,
		       16, 1, out, outlen, true);
	return -EIO;
}

static void *lz4_map_inplace_data(struct z_erofs_decompress_req *rq)
{
	const unsigned int nr =
		PAGE_ALIGN(rq->pageofs_out + rq->outputsize) >> PAGE_SHIFT;

	if (rq->overlapped_decoding) {
		if (rq->out[nr - 1] == rq->in[0])
			return kmap_atomic(rq->in[0]);

		rq->overlapped_decoding = false;
	}

	return generic_map_inplace_data(rq);
}

static struct z_erofs_decompressor decompressors[] = {
	[Z_EROFS_COMPRESSION_SHIFTED] = {
		.name = "shifted"
	},
	[Z_EROFS_COMPRESSION_LZ4] = {
		.prepare_destpages = lz4_prepare_destpages,
		.map_inplace_data = lz4_map_inplace_data,
		.unmap_inplace_data = generic_unmap_inplace_data,
		.decompress = lz4_decompress,
		.name = "lz4"
	},
};

static void copy_from_pcpubuf(struct page **out, const char *dst,
			      unsigned short pageofs_out,
			      unsigned int outputsize)
{
	const char *end = dst + outputsize;
	const unsigned int righthalf = PAGE_SIZE - pageofs_out;
	const char *cur = dst - pageofs_out;

	while (cur < end) {
		struct page *const page = *out++;

		if (page) {
			char *buf = kmap_atomic(page);

			if (cur >= dst) {
				memcpy(buf, cur, min_t(uint, PAGE_SIZE,
						       end - cur));
			} else {
				memcpy(buf + pageofs_out, cur + pageofs_out,
				       min_t(uint, righthalf, end - cur));
			}
			kunmap_atomic(buf);
		}
		cur += PAGE_SIZE;
	}
}

static int decompress_generic(struct z_erofs_decompress_req *rq,
			      struct list_head *pagepool)
{
	const unsigned int nrpages_out =
		PAGE_ALIGN(rq->pageofs_out + rq->outputsize) >> PAGE_SHIFT;
	const struct z_erofs_decompressor *alg = decompressors + rq->alg;
	unsigned int dst_maptype;
	void *src, *dst;
	int ret;

	if (nrpages_out == 1 && !rq->inplace_io) {
		DBG_BUGON(!*rq->out);
		dst = kmap_atomic(*rq->out);
		dst_maptype = 0;
		goto dstmap_out;
	}

	/*
	 * For the case of small output size (especially much less
	 * than PAGE_SIZE), memcpy the decompressed data rather than
	 * compressed data is preferred.
	 */
	if (rq->outputsize <= PAGE_SIZE) {
		dst = erofs_get_pcpubuf(0);

		src = kmap_atomic(*rq->in);
		ret = alg->decompress(src, dst, PAGE_SIZE, rq->outputsize,
				      rq->partial_decoding);
		kunmap_atomic(src);

		copy_from_pcpubuf(rq->out, dst, rq->pageofs_out,
				  rq->outputsize);
		erofs_put_pcpubuf(dst, 0);
		return ret;
	}

	ret = alg->prepare_destpages(rq, pagepool);
	if (ret < 0) {
		return ret;
	} else if (ret) {
		dst = page_address(*rq->out);
		dst_maptype = 1;
		goto dstmap_out;
	}

	if (nrpages_out <= erofs_lock_pcpu_vm_area(0, nrpages_out)) {
		dst = erofs_map_pcpu_vm_area(0, rq->out, nrpages_out);
		dst_maptype = 2;
	} else {
		erofs_unlock_pcpu_vm_area(0);

		dst = erofs_vmap(rq->out, nrpages_out);
		if (!dst)
			return -ENOMEM;
		dst_maptype = 3;
	}

dstmap_out:
	if (rq->inplace_io)
		src = alg->map_inplace_data(rq);
	else
		src = kmap_atomic(*rq->in);

	ret = alg->decompress(src, dst + rq->pageofs_out, PAGE_SIZE,
			      rq->outputsize, rq->partial_decoding);

	if (rq->inplace_io)
		alg->unmap_inplace_data(rq, src);
	else
		kunmap_atomic(src);

	if (!dst_maptype)
		kunmap_atomic(dst);
	else if (dst_maptype == 2)
		erofs_unlock_pcpu_vm_area(0);
	else if (dst_maptype == 3)
		erofs_vunmap(dst, nrpages_out);
	return ret;
}

static int shifted_decompress(const struct z_erofs_decompress_req *rq,
			      struct list_head *pagepool)
{
	const unsigned int nrpages_out =
		PAGE_ALIGN(rq->pageofs_out + rq->outputsize) >> PAGE_SHIFT;
	const unsigned int righthalf = PAGE_SIZE - rq->pageofs_out;
	unsigned char *src, *dst;

	if (nrpages_out > 2) {
		DBG_BUGON(1);
		return -EIO;
	}

	if (rq->out[0] == *rq->in) {
		DBG_BUGON(nrpages_out != 1);
		return 0;
	}

	src = kmap_atomic(*rq->in);
	if (!rq->out[0]) {
		dst = NULL;
	} else {
		dst = kmap_atomic(rq->out[0]);
		memcpy(dst + rq->pageofs_out, src, righthalf);
	}

	if (rq->out[1] == *rq->in) {
		memmove(src, src + righthalf, rq->pageofs_out);
	} else if (nrpages_out == 2) {
		if (dst)
			kunmap_atomic(dst);
		DBG_BUGON(!rq->out[1]);
		dst = kmap_atomic(rq->out[1]);
		memcpy(dst, src + righthalf, rq->pageofs_out);
	}
	if (dst)
		kunmap_atomic(dst);
	kunmap_atomic(src);
	return 0;
}

int z_erofs_decompress(struct z_erofs_decompress_req *rq,
		       struct list_head *pagepool)
{
	if (rq->alg == Z_EROFS_COMPRESSION_SHIFTED)
		return shifted_decompress(rq, pagepool);
	return decompress_generic(rq, pagepool);
}

