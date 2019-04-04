// SPDX-License-Identifier: GPL-2.0
/*
 * linux/drivers/staging/erofs/compress.h
 *
 * Copyright (C) 2019 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Gao Xiang <gaoxiang25@huawei.com>
 */
#ifndef __EROFS_FS_COMPRESS_H
#define __EROFS_FS_COMPRESS_H

#include "internal.h"

/*
 * - 0x5A110C8D ('sallocated', Z_EROFS_MAPPING_STAGING) -
 * used to mark temporary allocated pages from other
 * file/cached pages and NULL mapping pages.
 */
#define Z_EROFS_MAPPING_STAGING         ((void *)0x5A110C8D)

/* compression algorithm supported */
#define Z_EROFS_COMPRESSION_SHIFTED	0
#define Z_EROFS_COMPRESSION_LZ4		1

int z_erofs_decompress(unsigned int algorithm,
		       struct page **in_pages, struct page **out_pages,
		       unsigned int pageofs_out, unsigned int outputsize,
		       struct list_head *pagepool,
		       bool overlapped, bool sparsed);

#endif

