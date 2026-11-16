// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpu.h>
#include <linux/crypto.h>

#include "zcomp.h"

static const char * const backends[] = {
	"lzo",
	"lzo-rle",
#if IS_ENABLED(CONFIG_CRYPTO_LZ4)
	"lz4",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4HC)
	"lz4hc",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_842)
	"842",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_ZSTD)
	"zstd",
#endif
	NULL
};

static void zcomp_cstrm_free(struct zcomp_cstrm *cstrm)
{
	if (!IS_ERR_OR_NULL(cstrm->tfm))
		crypto_free_comp(cstrm->tfm);
	if (cstrm->buffer)
		free_pages((unsigned long)cstrm->buffer, 1);
	kfree(cstrm);
}

static struct zcomp_cstrm *zcomp_cstrm_alloc(struct zcomp *comp)
{
	struct zcomp_cstrm *cstrm = kmalloc(sizeof(*cstrm), GFP_KERNEL);
	if (!cstrm)
		return NULL;

	cstrm->tfm = crypto_alloc_comp(comp->name, 0, 0);
	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	cstrm->buffer = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
	if (IS_ERR_OR_NULL(cstrm->tfm) || !cstrm->buffer) {
		zcomp_cstrm_free(cstrm);
		cstrm = NULL;
	}
	return cstrm;
}

static void zcomp_dstrm_free(struct zcomp_dstrm *dstrm)
{
	if (!IS_ERR_OR_NULL(dstrm->tfm))
		crypto_free_comp(dstrm->tfm);
	if (dstrm->tmpbuf)
		free_pages((unsigned long)dstrm->tmpbuf, 1);
	kfree(dstrm);
}

static struct zcomp_dstrm *zcomp_dstrm_alloc(struct zcomp *comp)
{
	struct zcomp_dstrm *dstrm = kmalloc(sizeof(*dstrm), GFP_KERNEL);
	if (!dstrm)
		return NULL;

	dstrm->tfm = crypto_alloc_comp(comp->name, 0, 0);
	/* allocate 2 pages for decompression spanning temporary buffer */
	dstrm->tmpbuf = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
	if (IS_ERR_OR_NULL(dstrm->tfm) || !dstrm->tmpbuf) {
		zcomp_dstrm_free(dstrm);
		dstrm = NULL;
	}
	return dstrm;
}

bool zcomp_available_algorithm(const char *comp)
{
	int i;

	i = __sysfs_match_string(backends, -1, comp);
	if (i >= 0)
		return true;

	/*
	 * Crypto does not ignore a trailing new line symbol,
	 * so make sure you don't supply a string containing
	 * one.
	 * This also means that we permit zcomp initialisation
	 * with any compressing algorithm known to crypto api.
	 */
	return crypto_has_comp(comp, 0, 0) == 1;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	bool known_algorithm = false;
	ssize_t sz = 0;
	int i = 0;

	for (; backends[i]; i++) {
		if (!strcmp(comp, backends[i])) {
			known_algorithm = true;
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", backends[i]);
		} else {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", backends[i]);
		}
	}

	/*
	 * Out-of-tree module known to crypto api or a missing
	 * entry in `backends'.
	 */
	if (!known_algorithm && crypto_has_comp(comp, 0, 0) == 1)
		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
				"[%s] ", comp);

	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

struct zcomp_cstrm *zcomp_cstrm_get(struct zcomp *comp)
{
	return *get_cpu_ptr(comp->cstream);
}

void zcomp_cstrm_put(struct zcomp *comp)
{
	put_cpu_ptr(comp->cstream);
}

struct zcomp_dstrm *zcomp_dstrm_get(struct zcomp *comp)
{
	return *get_cpu_ptr(comp->dstream);
}

void zcomp_dstrm_put(struct zcomp *comp)
{
	put_cpu_ptr(comp->dstream);
}

int zcomp_compress(struct zcomp_cstrm *zstrm,
		const void *src, unsigned int *dst_len)
{
	/*
	 * Our dst memory (zstrm->buffer) is always `2 * PAGE_SIZE' sized
	 * because sometimes we can endup having a bigger compressed data
	 * due to various reasons: for example compression algorithms tend
	 * to add some padding to the compressed buffer. Speaking of padding,
	 * comp algorithm `842' pads the compressed length to multiple of 8
	 * and returns -ENOSP when the dst memory is not big enough, which
	 * is not something that ZRAM wants to see. We can handle the
	 * `compressed_size > PAGE_SIZE' case easily in ZRAM, but when we
	 * receive -ERRNO from the compressing backend we can't help it
	 * anymore. To make `842' happy we need to tell the exact size of
	 * the dst buffer, zram_drv will take care of the fact that
	 * compressed buffer is too big.
	 */
	*dst_len = PAGE_SIZE * 2;

	return crypto_comp_compress(zstrm->tfm,
			src, PAGE_SIZE,
			zstrm->buffer, dst_len);
}

int zcomp_decompress(struct zcomp_dstrm *zstrm,
		const void *src, unsigned int src_len, void *dst)
{
	unsigned int dst_len = PAGE_SIZE;

	return crypto_comp_decompress(zstrm->tfm,
			src, src_len,
			dst, &dst_len);
}

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_cstrm *cstrm;
	struct zcomp_dstrm *dstrm;

	if (WARN_ON(*per_cpu_ptr(comp->cstream, cpu)))
		return 0;

	cstrm = zcomp_cstrm_alloc(comp);
	if (IS_ERR_OR_NULL(cstrm)) {
		pr_err("Can't allocate a compression stream\n");
		return -ENOMEM;
	}
	*per_cpu_ptr(comp->cstream, cpu) = cstrm;

	dstrm = zcomp_dstrm_alloc(comp);
	if (IS_ERR_OR_NULL(dstrm)) {
		pr_err("Can't allocate a decompression stream\n");
		zcomp_cstrm_free(cstrm);
		*per_cpu_ptr(comp->cstream, cpu) = NULL;
		return -ENOMEM;
	}
	*per_cpu_ptr(comp->dstream, cpu) = dstrm;
	return 0;
}

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_cstrm *cstrm;
	struct zcomp_dstrm *dstrm;

	cstrm = *per_cpu_ptr(comp->cstream, cpu);
	if (!IS_ERR_OR_NULL(cstrm))
		zcomp_cstrm_free(cstrm);
	*per_cpu_ptr(comp->cstream, cpu) = NULL;

	dstrm = *per_cpu_ptr(comp->dstream, cpu);
	if (!IS_ERR_OR_NULL(dstrm))
		zcomp_dstrm_free(dstrm);
	*per_cpu_ptr(comp->dstream, cpu) = NULL;
	return 0;
}

static int zcomp_init(struct zcomp *comp)
{
	int ret;

	comp->cstream = alloc_percpu(struct zcomp_cstrm *);
	if (!comp->cstream)
		return -ENOMEM;

	comp->dstream = alloc_percpu(struct zcomp_dstrm *);
	if (!comp->dstream) {
		free_percpu(comp->cstream);
		return -ENOMEM;
	}

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto cleanup;
	return 0;

cleanup:
	free_percpu(comp->dstream);
	free_percpu(comp->cstream);
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	free_percpu(comp->dstream);
	free_percpu(comp->cstream);
	kfree(comp);
}

/*
 * search available compressors for requested algorithm.
 * allocate new zcomp and initialize it. return compressing
 * backend pointer or ERR_PTR if things went bad. ERR_PTR(-EINVAL)
 * if requested algorithm is not supported, ERR_PTR(-ENOMEM) in
 * case of allocation error, or any other error potentially
 * returned by zcomp_init().
 */
struct zcomp *zcomp_create(const char *alg)
{
	struct zcomp *comp;
	int error;

	if (!zcomp_available_algorithm(alg))
		return ERR_PTR(-EINVAL);

	comp = kzalloc(sizeof(struct zcomp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->name = alg;
	error = zcomp_init(comp);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
