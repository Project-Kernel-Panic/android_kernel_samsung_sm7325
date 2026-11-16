/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_

#include <linux/spinlock.h>

struct zcomp_cstrm {
	/* compression buffer */
	void *buffer;
	struct crypto_comp *tfm;
};

struct zcomp_dstrm {
	/* decompression buffer / local copy for spanned read */
	void *tmpbuf;
	struct crypto_comp *tfm;
};

/* dynamic per-device compression frontend */
struct zcomp {
	struct zcomp_cstrm * __percpu *cstream;
	struct zcomp_cstrm *recomp_cstream;
	spinlock_t recomp_lock;
	struct zcomp_dstrm * __percpu *dstream;
	const char *name;
	struct hlist_node node;
};

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_available_show(const char *comp, char *buf);
bool zcomp_available_algorithm(const char *comp);

struct zcomp *zcomp_create(const char *alg, bool is_recomp);
void zcomp_destroy(struct zcomp *comp);

struct zcomp_cstrm *zcomp_cstrm_get(struct zcomp *comp);
void zcomp_cstrm_put(struct zcomp *comp);

struct zcomp_dstrm *zcomp_dstrm_get(struct zcomp *comp);
void zcomp_dstrm_put(struct zcomp *comp);

int zcomp_compress(struct zcomp_cstrm *zstrm,
		const void *src, unsigned int *dst_len);

int zcomp_decompress(struct zcomp_dstrm *zstrm,
		const void *src, unsigned int src_len, void *dst);

bool zcomp_set_max_streams(struct zcomp *comp, int num_strm);
#endif /* _ZCOMP_H_ */
