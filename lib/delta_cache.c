/*
 * Copyright (c) 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <siphash.h>

#include "got_compat.h"

#include "got_object.h"
#include "got_error.h"

#include "got_lib_delta.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"
#include "got_lib_delta_cache.h"

#ifndef nitems
#define nitems(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

#define GOT_DELTA_CACHE_MIN_BUCKETS	64
#define GOT_DELTA_CACHE_MAX_BUCKETS	16384
#define GOT_DELTA_CACHE_MAX_CHAIN	2
#define GOT_DELTA_CACHE_MAX_DELTA_SIZE	2048

struct got_cached_delta {
	off_t offset;
	uint8_t *data;
	size_t len;
};

struct got_delta_cache_head {
	struct got_cached_delta entries[GOT_DELTA_CACHE_MAX_CHAIN];
	unsigned int nchain;
};

struct got_delta_cache {
	struct got_delta_cache_head *buckets;
	unsigned int nbuckets;
	unsigned int totelem;
	int cache_search;
	int cache_hit;
	int cache_miss;
	int cache_evict;
	int cache_toolarge;
	unsigned int flags;
#define GOT_DELTA_CACHE_F_NOMEM	0x01
	SIPHASH_KEY key;
};

const struct got_error *
got_delta_cache_alloc(struct got_delta_cache **new)
{
	const struct got_error *err;
	struct got_delta_cache *cache;

	*new = NULL;

	cache = calloc(1, sizeof(*cache));
	if (cache == NULL)
		return got_error_from_errno("calloc");

	cache->buckets = calloc(GOT_DELTA_CACHE_MIN_BUCKETS,
	    sizeof(cache->buckets[0]));
	if (cache->buckets == NULL) {
		err = got_error_from_errno("calloc");
		free(cache);
		return err;
	}
	cache->nbuckets = GOT_DELTA_CACHE_MIN_BUCKETS;

	arc4random_buf(&cache->key, sizeof(cache->key));
	*new = cache;
	return NULL;
}

void
got_delta_cache_free(struct got_delta_cache *cache)
{
	struct got_cached_delta *delta;
	unsigned int i;

#ifdef GOT_OBJ_CACHE_DEBUG
	fprintf(stderr, "%s: delta cache: %u elements, %d searches, %d hits, "
	    "%d missed, %d evicted, %d too large\n", getprogname(),
	    cache->totelem, cache->cache_search, cache->cache_hit,
	    cache->cache_miss, cache->cache_evict, cache->cache_toolarge);
#endif
	for (i = 0; i < cache->nbuckets; i++) {
		struct got_delta_cache_head *head;
		int j;
		head = &cache->buckets[i];
		for (j = 0; j < head->nchain; j++) {
			delta = &head->entries[j];
			free(delta->data);
		}
	}
	free(cache->buckets);
	free(cache);
}

static uint64_t
delta_cache_hash(struct got_delta_cache *cache, off_t delta_offset)
{
	return SipHash24(&cache->key, &delta_offset, sizeof(delta_offset));
}

#ifndef GOT_NO_OBJ_CACHE
static const struct got_error *
delta_cache_resize(struct got_delta_cache *cache, unsigned int nbuckets)
{
	struct got_delta_cache_head *buckets;
	size_t i;

	buckets = calloc(nbuckets, sizeof(buckets[0]));
	if (buckets == NULL) {
		if (errno != ENOMEM)
			return got_error_from_errno("calloc");
		/* Proceed with our current amount of hash buckets. */
		cache->flags |= GOT_DELTA_CACHE_F_NOMEM;
		return NULL;
	}

	arc4random_buf(&cache->key, sizeof(cache->key));

	for (i = 0; i < cache->nbuckets; i++) {
		unsigned int j;
		for (j = 0; j < cache->buckets[i].nchain; j++) {
			struct got_delta_cache_head *head;
			struct got_cached_delta *delta;
			uint64_t idx;
			delta = &cache->buckets[i].entries[j];
			idx = delta_cache_hash(cache, delta->offset) % nbuckets;
			head = &buckets[idx];
			if (head->nchain < nitems(head->entries)) {
				struct got_cached_delta *new_delta;
				new_delta = &head->entries[head->nchain];
				memcpy(new_delta, delta, sizeof(*new_delta));
				head->nchain++;
			} else
				free(delta->data);
		}
	}

	free(cache->buckets);
	cache->buckets = buckets;
	cache->nbuckets = nbuckets;
	return NULL;
}

static const struct got_error *
delta_cache_grow(struct got_delta_cache *cache)
{
	unsigned int nbuckets;

	if ((cache->flags & GOT_DELTA_CACHE_F_NOMEM) ||
	    cache->nbuckets == GOT_DELTA_CACHE_MAX_BUCKETS)
		return NULL;

	if (cache->nbuckets >= GOT_DELTA_CACHE_MAX_BUCKETS / 2)
		nbuckets = GOT_DELTA_CACHE_MAX_BUCKETS;
	else
		nbuckets = cache->nbuckets * 2;

	return delta_cache_resize(cache, nbuckets);
}
#endif

const struct got_error *
got_delta_cache_add(struct got_delta_cache *cache,
    off_t delta_data_offset, uint8_t *delta_data, size_t delta_len)
{
#ifdef GOT_NO_OBJ_CACHE
	return got_error(GOT_ERR_NO_SPACE);
#else
	const struct got_error *err = NULL;
	struct got_cached_delta *delta;
	struct got_delta_cache_head *head;
	uint64_t idx;

	if (delta_len > GOT_DELTA_CACHE_MAX_DELTA_SIZE) {
		cache->cache_toolarge++;
		return got_error(GOT_ERR_NO_SPACE);
	}

	if (cache->nbuckets * 3 < cache->totelem * 4) {
		err = delta_cache_grow(cache);
		if (err)
			return err;
	}

	idx = delta_cache_hash(cache, delta_data_offset) % cache->nbuckets;
	head = &cache->buckets[idx];
	if (head->nchain >= nitems(head->entries)) {
		delta = &head->entries[head->nchain - 1];
		free(delta->data);
		memset(delta, 0, sizeof(*delta));
		head->nchain--;
	}

	delta = &head->entries[head->nchain];
	delta->offset = delta_data_offset;
	delta->data = delta_data;
	delta->len = delta_len;
	head->nchain++;
	cache->totelem++;

	return NULL;
#endif
}

void
got_delta_cache_get(uint8_t **delta_data, size_t *delta_len,
    struct got_delta_cache *cache, off_t delta_data_offset)
{
	uint64_t idx;
	struct got_delta_cache_head *head;
	struct got_cached_delta *delta;
	int i;

	idx = delta_cache_hash(cache, delta_data_offset) % cache->nbuckets;
	head = &cache->buckets[idx];

	cache->cache_search++;
	*delta_data = NULL;
	*delta_len = 0;
	for (i = 0; i < head->nchain; i++) {
		delta = &head->entries[i];
		if (delta->offset != delta_data_offset)
			continue;
		cache->cache_hit++;
		if (i > 0) {
			struct got_cached_delta tmp;
			memcpy(&tmp, &head->entries[0], sizeof(tmp));
			memcpy(&head->entries[0], &head->entries[i],
			    sizeof(head->entries[0]));
			memcpy(&head->entries[i], &tmp,
			    sizeof(head->entries[i]));
			delta = &head->entries[0];
		}
		*delta_data = delta->data;
		*delta_len = delta->len;
		return;
	}

	cache->cache_miss++;
}
