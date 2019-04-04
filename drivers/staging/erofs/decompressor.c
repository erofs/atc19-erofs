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
	int (*prepare_bounce_pages)(struct page **pages, unsigned int nr,
				    struct list_head *pagepool);
	void *(*map_inplace_pages)(struct page **out, struct page **in,
				   unsigned short pageofs_out,
				   unsigned int outputsize,
				   unsigned short pageofs_in,
				   unsigned int inputsize);
	void (*unmap_inplace_pages)(void *src);
	int (*decompress_partial)(const char *src, char *dst,
				  unsigned int srclen, unsigned int dstlen);
	char *name;
};

#define LZ4_MAX_DISTANCE_PAGES	(65536 >> PAGE_SHIFT)

static int lz4_prepare_bounce_pages(struct page **pages, unsigned int nr,
				    struct list_head *pagepool)
{
	struct page *availables[LZ4_MAX_DISTANCE_PAGES] = { NULL };
	unsigned long unused = 0;
	unsigned int i, j;

	for (i = 0; i < nr; ++i) {
		j = i & (LZ4_MAX_DISTANCE_PAGES - 1);
		unused |= (unsigned long)(!!availables[j]) << j;

		if (pages[i])
			continue;

		if (unused) {
			j = __ffs(unused);
			get_page(availables[j]);
		} else {
			availables[j] = erofs_allocpage(pagepool, GFP_KERNEL,
							true, false);
			if (!availables[j])
				return -ENOMEM;
			availables[j]->mapping = Z_EROFS_MAPPING_STAGING;
		}
		pages[i] = availables[j];
		__clear_bit(j, &unused);
	}
	return 0;
}

static void *generic_map_inplace_pages(struct page **out, struct page **in,
				       unsigned short pageofs_out,
				       unsigned int outputsize,
				       unsigned short pageofs_in,
				       unsigned int inputsize)
{
	/*
	 * if in-place decompression is ongoing, those decompressed
	 * pages should be copied in order to avoid being overlapped.
	 */
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

static void generic_unmap_inplace_pages(void *src)
{
	erofs_put_pcpubuf(src, 0);
}

static int lz4_decompress_partial(const char *in, char *out,
				  unsigned int inlen, unsigned int outlen)
{
	int ret;

	ret = LZ4_decompress_safe_partial(in, out, inlen, outlen, outlen);
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

static struct z_erofs_decompressor decompressors[] = {
	[Z_EROFS_COMPRESSION_SHIFTED] = {
		.name = "shifted"
	},
	[Z_EROFS_COMPRESSION_LZ4] = {
		.prepare_bounce_pages = lz4_prepare_bounce_pages,
		.map_inplace_pages = generic_map_inplace_pages,
		.unmap_inplace_pages = generic_unmap_inplace_pages,
		.decompress_partial = lz4_decompress_partial,
		.name = "lz4"
	},
};

struct z_erofs_decompress_handle {
	struct page **in, **out;
	unsigned short pageofs_out;
	unsigned int outputsize;
	/* indicate the algorithm will be used for decompression */
	unsigned int alg;
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

static int decompress_generic(const struct z_erofs_decompress_handle *dh,
			      struct list_head *pagepool, bool overlapped,
			      bool sparsed)
{
	const unsigned int nrpages_out =
		PAGE_ALIGN(dh->pageofs_out + dh->outputsize) >> PAGE_SHIFT;
	const struct z_erofs_decompressor *alg = decompressors + dh->alg;
	unsigned int dst_maptype;
	void *src, *dst;
	int ret;

	if (nrpages_out == 1 && !overlapped) {
		DBG_BUGON(!*dh->out);
		dst = kmap_atomic(*dh->out);
		dst_maptype = 0;
	} else {
		/*
		 * For the case of small output size (especially much less
		 * than PAGE_SIZE), memcpy the decompressed data rather than
		 * compressed data is preferred.
		 */
		if (dh->outputsize <= PAGE_SIZE) {
			dst = erofs_get_pcpubuf(0);

			src = kmap_atomic(*dh->in);
			ret = alg->decompress_partial(src, dst, PAGE_SIZE,
						      dh->outputsize);
			kunmap_atomic(src);

			copy_from_pcpubuf(dh->out, dst, dh->pageofs_out,
					  dh->outputsize);
			erofs_put_pcpubuf(dst, 0);
			return ret;
		}

		if (sparsed) {
			ret = alg->prepare_bounce_pages(dh->out, nrpages_out,
							pagepool);
			if (ret)
				return ret;
		}

		if (nrpages_out <= erofs_lock_pcpu_vm_area(0, nrpages_out)) {
			dst = erofs_map_pcpu_vm_area(0, dh->out, nrpages_out);
			dst_maptype = 1;
		} else {
			erofs_unlock_pcpu_vm_area(0);

			dst = erofs_vmap(dh->out, nrpages_out);
			if (!dst)
				return -ENOMEM;
			dst_maptype = 2;
		}
	}

	if (overlapped)
		src = alg->map_inplace_pages(dh->out, dh->in, dh->pageofs_out,
					     dh->outputsize, 0, PAGE_SIZE);
	else
		src = kmap_atomic(*dh->in);

	ret = alg->decompress_partial(src, dst + dh->pageofs_out,
				      PAGE_SIZE, dh->outputsize);
	if (overlapped)
		alg->unmap_inplace_pages(src);
	else
		kunmap_atomic(src);

	if (!dst_maptype)
		kunmap_atomic(dst);
	else if (dst_maptype == 1)
		erofs_unlock_pcpu_vm_area(0);
	else
		erofs_vunmap(dst, nrpages_out);
	return ret;
}

static int shifted_decompress(const struct z_erofs_decompress_handle *dh,
			      struct list_head *pagepool, bool overlapped)
{
	const unsigned int nrpages_out =
		PAGE_ALIGN(dh->pageofs_out + dh->outputsize) >> PAGE_SHIFT;
	const unsigned int righthalf = PAGE_SIZE - dh->pageofs_out;
	unsigned char *src, *dst;

	if (nrpages_out > 2) {
		DBG_BUGON(1);
		return -EIO;
	}

	if (dh->out[0] == *dh->in) {
		DBG_BUGON(nrpages_out != 1);
		DBG_BUGON(dh->pageofs_out);
		return 0;
	}

	src = kmap_atomic(*dh->in);
	if (!dh->out[0]) {
		dst = NULL;
	} else {
		dst = kmap_atomic(dh->out[0]);
		memcpy(dst + dh->pageofs_out, src, righthalf);
	}

	if (dh->out[1] == *dh->in) {
		memmove(src, src + righthalf, dh->pageofs_out);
	} else if (nrpages_out == 2) {
		if (dst)
			kunmap_atomic(dst);
		DBG_BUGON(!dh->out[1]);
		dst = kmap_atomic(dh->out[1]);
		memcpy(dst, src + righthalf, dh->pageofs_out);
	}
	if (dst)
		kunmap_atomic(dst);
	kunmap_atomic(src);
	return 0;
}

int z_erofs_decompress(unsigned int algorithm,
		       struct page **in_pages, struct page **out_pages,
		       unsigned int pageofs_out, unsigned int outputsize,
		       struct list_head *pagepool,
		       bool overlapped, bool sparsed)
{
	const struct z_erofs_decompress_handle dh = {
		.in = in_pages, .out = out_pages,
		.pageofs_out = pageofs_out,
		.outputsize = outputsize,
		.alg = algorithm,
	};

	if (algorithm == Z_EROFS_COMPRESSION_SHIFTED)
		return shifted_decompress(&dh, pagepool, overlapped);
	return decompress_generic(&dh, pagepool, overlapped, sparsed);
}

