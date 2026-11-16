// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpu.h>
#include <linux/vmalloc.h>

#include "zcomp.h"

#include "backend_lzo.h"
#include "backend_lzorle.h"
#include "backend_lz4.h"
#include "backend_lz4hc.h"
#include "backend_zstd.h"
#include "backend_deflate.h"
#include "backend_842.h"

static const struct zcomp_ops *backends[] = {
#if IS_ENABLED(CONFIG_LZO_COMPRESS)
	&backend_lzorle,
	&backend_lzo,
#endif
#if IS_ENABLED(CONFIG_LZ4_COMPRESS)
	&backend_lz4,
#endif
#if IS_ENABLED(CONFIG_LZ4HC_COMPRESS)
	&backend_lz4hc,
#endif
#if IS_ENABLED(CONFIG_ZSTD_COMPRESS)
	&backend_zstd,
#endif
#if IS_ENABLED(CONFIG_ZLIB_DEFLATE)
	&backend_deflate,
#endif
#if IS_ENABLED(CONFIG_842_COMPRESS)
	&backend_842,
#endif
	NULL
};

static void zcomp_strm_free(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	if (!zstrm)
		return;
	if (comp->ops && comp->ops->destroy_ctx)
		comp->ops->destroy_ctx(&zstrm->ctx);
	vfree(zstrm->buffer);
	zstrm->buffer = NULL;
}

static int zcomp_strm_init(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	int ret;

	ret = comp->ops->create_ctx(comp->params, &zstrm->ctx);
	if (ret)
		return ret;

	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	zstrm->buffer = vzalloc(2 * PAGE_SIZE);
	if (!zstrm->buffer) {
		zcomp_strm_free(comp, zstrm);
		return -ENOMEM;
	}
	return 0;
}

static const struct zcomp_ops *lookup_backend_ops(const char *comp)
{
	int i = 0;

	while (backends[i]) {
		if (sysfs_streq(comp, backends[i]->name))
			break;
		i++;
	}
	return backends[i];
}

bool zcomp_available_algorithm(const char *comp)
{
	return lookup_backend_ops(comp) != NULL;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	ssize_t sz = 0;
	int i;

	for (i = 0; backends[i]; i++) {
		if (!strcmp(comp, backends[i]->name)) {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", backends[i]->name);
		} else {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", backends[i]->name);
		}
	}

	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp)
{
	preempt_disable();
	return *this_cpu_ptr(comp->stream);
}

void zcomp_stream_put(struct zcomp *comp)
{
	preempt_enable();
}

int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		   const void *src, unsigned int *dst_len)
{
	struct zcomp_req req = {
		.src = src,
		.dst = zstrm->buffer,
		.src_len = PAGE_SIZE,
		.dst_len = 2 * PAGE_SIZE,
	};
	int ret;

	ret = comp->ops->compress(comp->params, &zstrm->ctx, &req);
	if (!ret)
		*dst_len = req.dst_len;
	return ret;
}

int zcomp_decompress(struct zcomp *comp, struct zcomp_strm *zstrm,
		     const void *src, unsigned int src_len, void *dst)
{
	struct zcomp_req req = {
		.src = src,
		.dst = dst,
		.src_len = src_len,
		.dst_len = PAGE_SIZE,
	};

	return comp->ops->decompress(comp->params, &zstrm->ctx, &req);
}

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm;
	int ret;

	zstrm = kzalloc_node(sizeof(*zstrm), GFP_KERNEL, cpu_to_node(cpu));
	if (!zstrm)
		return -ENOMEM;

	ret = zcomp_strm_init(comp, zstrm);
	if (ret) {
		pr_err("Can't allocate a compression stream\n");
		kfree(zstrm);
		return ret;
	}
	*per_cpu_ptr(comp->stream, cpu) = zstrm;
	return 0;
}

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_strm *zstrm;

	zstrm = *per_cpu_ptr(comp->stream, cpu);
	if (zstrm) {
		zcomp_strm_free(comp, zstrm);
		kfree(zstrm);
		*per_cpu_ptr(comp->stream, cpu) = NULL;
	}
	return 0;
}

static int zcomp_init(struct zcomp *comp, struct zcomp_params *params)
{
	int ret;

	comp->stream = alloc_percpu(struct zcomp_strm *);
	if (!comp->stream)
		return -ENOMEM;

	comp->params = params;
	if (comp->ops->setup_params) {
		ret = comp->ops->setup_params(comp->params);
		if (ret)
			goto cleanup;
	}

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto cleanup;

	return 0;

cleanup:
	if (comp->ops->release_params)
		comp->ops->release_params(comp->params);
	free_percpu(comp->stream);
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	if (!comp)
		return;
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (comp->ops && comp->ops->release_params)
		comp->ops->release_params(comp->params);
	free_percpu(comp->stream);
	kfree(comp);
}

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params)
{
	struct zcomp *comp;
	int error;

	BUILD_BUG_ON(ARRAY_SIZE(backends) <= 1);

	comp = kzalloc(sizeof(struct zcomp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->ops = lookup_backend_ops(alg);
	if (!comp->ops) {
		kfree(comp);
		return ERR_PTR(-EINVAL);
	}

	error = zcomp_init(comp, params);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
