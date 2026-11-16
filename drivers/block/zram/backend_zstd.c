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
#ifndef zstd_min_clevel
#define zstd_min_clevel() 1
#endif
#ifndef zstd_max_clevel
#define zstd_max_clevel() ZSTD_maxCLevel()
#endif

struct zstd_ctx {
	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;
	void *cctx_mem;
	void *dctx_mem;
};

struct zstd_params {
	ZSTD_CDict *cdict;
	void *cdict_mem;
	ZSTD_DDict *ddict;
	void *ddict_mem;
	ZSTD_parameters cprm;
};

static void zstd_release_params(struct zcomp_params *params)
{
	struct zstd_params *zp = params->drv_data;

	params->drv_data = NULL;
	if (!zp)
		return;

	vfree(zp->cdict_mem);
	vfree(zp->ddict_mem);
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

	zp->cprm = ZSTD_getParams(params->level, PAGE_SIZE, 0);

	if (params->dict_sz > 0 && params->dict) {
		size_t sz = ZSTD_CDictWorkspaceBound(zp->cprm.cParams);
		zp->cdict_mem = vzalloc(sz);
		if (!zp->cdict_mem)
			goto error;
		zp->cdict = ZSTD_initCDict(params->dict, params->dict_sz,
					   zp->cprm, zp->cdict_mem, sz);
		if (!zp->cdict)
			goto error;

		sz = ZSTD_DDictWorkspaceBound();
		zp->ddict_mem = vzalloc(sz);
		if (!zp->ddict_mem)
			goto error;
		zp->ddict = ZSTD_initDDict(params->dict, params->dict_sz,
					   zp->ddict_mem, sz);
		if (!zp->ddict)
			goto error;
	}

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
	struct zstd_params *zp = params->drv_data;
	size_t sz;

	zctx = kzalloc(sizeof(*zctx), GFP_KERNEL);
	if (!zctx)
		return -ENOMEM;

	ctx->context = zctx;

	sz = ZSTD_CCtxWorkspaceBound(zp->cprm.cParams);
	zctx->cctx_mem = vzalloc(sz);
	if (!zctx->cctx_mem)
		goto error;

	zctx->cctx = ZSTD_initCCtx(zctx->cctx_mem, sz);
	if (!zctx->cctx)
		goto error;

	sz = ZSTD_DCtxWorkspaceBound();
	zctx->dctx_mem = vzalloc(sz);
	if (!zctx->dctx_mem)
		goto error;

	zctx->dctx = ZSTD_initDCtx(zctx->dctx_mem, sz);
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

	if (!zp->cdict)
		ret = ZSTD_compressCCtx(zctx->cctx, req->dst, req->dst_len,
					req->src, req->src_len, zp->cprm);
	else
		ret = ZSTD_compress_usingCDict(zctx->cctx, req->dst,
					       req->dst_len, req->src,
					       req->src_len,
					       zp->cdict);
	if (ZSTD_isError(ret))
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int zstd_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	if (!zp->ddict)
		ret = ZSTD_decompressDCtx(zctx->dctx, req->dst, req->dst_len,
					  req->src, req->src_len);
	else
		ret = ZSTD_decompress_usingDDict(zctx->dctx, req->dst,
						 req->dst_len, req->src,
						 req->src_len, zp->ddict);
	if (ZSTD_isError(ret))
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
