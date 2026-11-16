// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) "zstd: " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "backend_zstd.h"

#ifndef zstd_default_clevel
#define zstd_default_clevel() 3
#endif

struct zstd_ctx {
	zstd_cctx *cctx;
	zstd_dctx *dctx;
	void *cctx_mem;
	void *dctx_mem;
};

struct zstd_params {
	zstd_parameters cprm;
};

static void zstd_release_params(struct zcomp_params *params)
{
	struct zstd_params *zp = params->drv_data;

	params->drv_data = NULL;
	kfree(zp);
}

static int zstd_setup_params(struct zcomp_params *params)
{
	struct zstd_params *zp;

	zp = kzalloc(sizeof(*zp), GFP_KERNEL);
	if (!zp)
		return -ENOMEM;

	params->drv_data = zp;
	if (params->level == ZCOMP_PARAM_NOT_SET) {
		params->level = zstd_default_clevel();
	} else if (params->level < zstd_min_clevel() ||
		   params->level > zstd_max_clevel()) {
		pr_err("invalid compression level %d\n", params->level);
		goto error;
	}

	if (params->dict_sz > 0) {
		pr_err("dictionaries not supported\n");
		goto error;
	}

	zp->cprm = zstd_get_params(params->level, PAGE_SIZE);
	return 0;

error:
	return -EINVAL;
}

static void zstd_destroy(struct zcomp_ctx *ctx)
{
	struct zstd_ctx *zctx = ctx->context;

	if (!zctx)
		return;

	vfree(zctx->cctx_mem);
	vfree(zctx->dctx_mem);
	kfree(zctx);
}

static int zstd_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct zstd_ctx *zctx;
	zstd_parameters prm;
	size_t sz;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;

	ctx->context = zctx;

	prm = zstd_get_params(params->level, PAGE_SIZE);
	sz = zstd_cctx_workspace_bound(&prm.cParams);
	zctx->cctx_mem = vzalloc(sz);
	if (!zctx->cctx_mem)
		goto error;

	zctx->cctx = zstd_init_cctx(zctx->cctx_mem, sz);
	if (!zctx->cctx)
		goto error;

	sz = zstd_dctx_workspace_bound();
	zctx->dctx_mem = vzalloc(sz);
	if (!zctx->dctx_mem)
		goto error;

	zctx->dctx = zstd_init_dctx(zctx->dctx_mem, sz);
	if (!zctx->dctx)
		goto error;

	return 0;

error:
	zstd_destroy(ctx);
	return -EINVAL;
}

static int zstd_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			 struct zcomp_req *req)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	ret = zstd_compress_cctx(zctx->cctx, req->dst, req->dst_len,
				 req->src, req->src_len, &zp->cprm);
	if (zstd_is_error(ret))
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int zstd_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	ret = zstd_decompress_dctx(zctx->dctx, req->dst, req->dst_len,
				   req->src, req->src_len);
	if (zstd_is_error(ret))
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_zstd = {
	.compress	= zstd_compress,
	.decompress	= zstd_decompress,
	.create_ctx	= zstd_create,
	.destroy_ctx	= zstd_destroy,
	.setup_params	= zstd_setup_params,
	.release_params	= zstd_release_params,
	.name		= "zstd",
};
