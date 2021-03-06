/*
 * twemcache - Twitter memcached.
 * Copyright (c) 2012, Twitter, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the name of the Twitter nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>

#include <mc_core.h>
#include <mc_sqltrig.h>

extern struct settings settings;

/*
 * We only reposition items in the lru q if they haven't been
 * repositioned in this many seconds. That saves us from churning
 * on frequently-accessed items
 */
#define ITEM_UPDATE_INTERVAL    60

#define ITEM_LRUQ_MAX_TRIES     50

#define ILEASE_MEET_QLEASE 0

/* 2MB is the maximum response size for 'cachedump' command */
#define ITEM_CACHEDUMP_MEMLIMIT (2 * MB)

pthread_mutex_t cache_lock;                     /* lock protecting lru q and hash */
pthread_mutex_t configuration_lock;
struct item_tqh item_lruq[SLABCLASS_MAX_IDS];   /* lru q of items */
struct item_tqh reserved_item_lruq[SLABCLASS_MAX_IDS];   /* lru q of reserved items */
static uint64_t cas_id;                         /* unique cas id */

/*
 * Returns the next cas id for a new item. Minimum cas value
 * is 1 and the maximum cas value is UINT64_MAX
 */
static uint64_t
item_next_cas(void)
{
	if (settings.use_cas) {
		return ++cas_id;
	}

	return 0ULL;
}

static bool
item_expired(struct item *it)
{
	ASSERT(it->magic == ITEM_MAGIC);

	return (it->exptime > 0 && it->exptime < time_now()) ? true : false;
}

void
item_init(void)
{
	uint8_t i;

	log_debug(LOG_DEBUG, "item hdr size %d", ITEM_HDR_SIZE);

	pthread_mutex_init(&cache_lock, NULL);
	pthread_mutex_init(&configuration_lock, NULL);

	for (i = SLABCLASS_MIN_ID; i <= SLABCLASS_MAX_ID; i++) {
		TAILQ_INIT(&item_lruq[i]);
		TAILQ_INIT(&reserved_item_lruq[i]);
	}

	cas_id = 0ULL;
}

void
item_deinit(void)
{
}

/*
 * Get start location of item payload
 */
char *
item_data(struct item *it)
{
	char *data;

	ASSERT(it->magic == ITEM_MAGIC);

	if (item_is_raligned(it)) {
		data = (char *)it + slab_item_size(it->id) - it->nbyte;
	} else {
		data = it->end + it->nkey + 1; /* 1 for terminal '\0' in key */
		if (item_has_cas(it)) {
			data += sizeof(uint64_t);
		}
	}

	return data;
}

/*
 * Get the slab that contains this item.
 */
struct slab *
item_2_slab(struct item *it)
{
	struct slab *slab;

	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(it->offset < settings.slab_size);

	slab = (struct slab *)((uint8_t *)it - it->offset);

	ASSERT(slab->magic == SLAB_MAGIC);

	return slab;
}

static void
item_acquire_refcount(struct item *it)
{
	ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
	ASSERT(it->magic == ITEM_MAGIC);

	it->refcount++;
	slab_acquire_refcount(item_2_slab(it));
}

static void
item_release_refcount(struct item *it)
{
	ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(it->refcount > 0);

	it->refcount--;
	slab_release_refcount(item_2_slab(it));
}

void
item_hdr_init(struct item *it, uint32_t offset, uint8_t id)
{
	ASSERT(offset >= SLAB_HDR_SIZE && offset < settings.slab_size);

	it->magic = ITEM_MAGIC;
	it->offset = offset;
	it->id = id;
	it->refcount = 0;
	it->flags = 0;
}

/*
 * Add an item to the tail of the lru q.
 *
 * Lru q is sorted in ascending time order - oldest to most recent. So
 * enqueuing at item to the tail of the lru q requires us to update its
 * last access time atime.
 *
 * The allocated flag indicates whether the item being re-linked is a newly
 * allocated or not. This is useful for updating the slab lruq, which can
 * choose to update only when a new item has been allocated (write-only) or
 * the opposite (read-only), or on both occasions (access-based).
 */
static void
item_link_q(struct item *it, bool allocated)
{
	uint8_t id = it->id;

	ASSERT(id >= SLABCLASS_MIN_ID && id <= SLABCLASS_MAX_ID);
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(!item_is_slabbed(it));

	log_debug(LOG_VVERB, "link q it '%.*s' at offset %"PRIu32" with flags "
			"%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
			it->flags, it->id);

	it->atime = time_now();
	if (item_is_lease_holder(it)) {
		TAILQ_INSERT_TAIL(&reserved_item_lruq[id], it, i_tqe);
		slab_reserved_lruq_touch(item_2_slab(it), allocated);
	} else {
		TAILQ_INSERT_TAIL(&item_lruq[id], it, i_tqe);
		slab_lruq_touch(item_2_slab(it), allocated);
	}

	stats_slab_incr(id, item_curr);
	stats_slab_incr_by(id, data_curr, item_size(it));
	stats_slab_incr_by(id, data_value_curr, it->nbyte);
}

/*
 * Remove the item from the lru q
 */
static void
item_unlink_q(struct item *it)
{
	uint8_t id = it->id;

	ASSERT(id >= SLABCLASS_MIN_ID && id <= SLABCLASS_MAX_ID);
	ASSERT(it->magic == ITEM_MAGIC);

	log_debug(LOG_VVERB, "unlink q it '%.*s' at offset %"PRIu32" with flags "
			"%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
			it->flags, it->id);

	if(item_is_lease_holder(it)) {
		TAILQ_REMOVE(&reserved_item_lruq[id], it, i_tqe);
	} else {
		TAILQ_REMOVE(&item_lruq[id], it, i_tqe);
	}


	stats_slab_decr(id, item_curr);
	stats_slab_decr_by(id, data_curr, item_size(it));
	stats_slab_decr_by(id, data_value_curr, it->nbyte);
}

/*
 * Make an item with zero refcount available for reuse by unlinking
 * it from the lru q and hash.
 *
 * Don't free the item yet because that would make it unavailable
 * for reuse.
 */
void
item_reuse(struct item *it)
{
	ASSERT(pthread_mutex_trylock(&cache_lock) != 0);
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(!item_is_slabbed(it));
	ASSERT(item_is_linked(it));

	if (item_is_lease_holder(it)) {
		ASSERT(it->refcount == 1);
		item_unset_pinned(it);
	} else
		ASSERT(it->refcount == 0);

	it->flags &= ~ITEM_LINKED;

	assoc_delete(item_key(it), it->nkey);
	item_unlink_q(it);

	stats_slab_incr(it->id, item_remove);
	stats_slab_settime(it->id, item_reclaim_ts, time_now());

	stats_thread_incr(items_unlink);

	log_debug(LOG_VERB, "reuse %s it '%.*s' at offset %"PRIu32" with id "
			"%"PRIu8" refcount ""%"PRIu32"", item_expired(it) ? "expired" : "evicted",
					it->nkey, item_key(it), it->offset, it->id, it->refcount);
}

/*
 * Find an unused (unreferenced) item from lru q.
 *
 * First try to find an expired item from the lru Q of item's slab
 * class; if all items are unexpired, return the one that is the
 * least recently used.
 *
 * We bound the search for an expired item in lru q, by only
 * traversing the oldest ITEM_LRUQ_MAX_TRIES items.
 */
static struct item *
item_get_from_lruq(uint8_t id, struct item_tqh *lruq)
{
	struct item *it;  /* expired item */
	struct item *uit; /* unexpired item */
	uint32_t tries;

	if (!settings.use_lruq) {
		return NULL;
	}

	for (tries = ITEM_LRUQ_MAX_TRIES, it = TAILQ_FIRST(&lruq[id]),
			uit = NULL;
			it != NULL && tries > 0;
			tries--, it = TAILQ_NEXT(it, i_tqe)) {

		//    	log_debug(LOG_VERB, "|| get it '%.*s' from LRU slab %"PRIu8, it->nkey, item_key(it), it->id);

		/* Skip an un-pinned item if it's refcount is larger than 0.
		 * Skip a pinned item if it's refcount is larger than 1 (pinned
		 *  items always have 1 refcount to prevent the slab from being
		 *  evicted).
		 */
		if (it->refcount != 0) {
			if (!item_is_lease_holder(it)) {
				log_debug(LOG_VERB, "skip it '%.*s' at offset %"PRIu32" with "
						"flags %02x id %"PRId8" refcount %"PRIu16"", it->nkey,
						item_key(it), it->offset, it->flags, it->id,
						it->refcount);
			}
			if (item_is_lease_holder(it) && it->refcount >= 2) {
				log_debug(LOG_VERB, "skip it '%.*s' at offset %"PRIu32" with "
						"flags %02x id %"PRId8" refcount %"PRIu16"", it->nkey,
						item_key(it), it->offset, it->flags, it->id,
						it->refcount);
			}
			continue;
		}

		if (item_expired(it)) {
			/* first expired item wins */
			return it;
		} else if (uit == NULL) {
			/* otherwise, get the lru unexpired item */
			uit = it;
			break;
		}
	}

	return uit;
}

uint8_t item_slabid(uint8_t nkey, uint32_t nbyte)
{
	size_t ntotal;
	uint8_t id;

	ntotal = item_ntotal(nkey, nbyte, settings.use_cas);

	id = slab_id(ntotal);
	if (id == SLABCLASS_INVALID_ID) {
		log_debug(LOG_NOTICE, "slab class id out of range with %"PRIu8" bytes "
				"key, %"PRIu32" bytes value and %zu item chunk size", nkey,
				nbyte, ntotal);
	}

	return id;
}

/*
 * Allocate an item. We allocate an item either by -
 *  1. Reusing an expired item from the lru Q of an item's slab class. Or,
 *  2. Consuming the next free item from slab of the item's an slab class
 *
 * On success we return the pointer to the allocated item. The returned item
 * is refcounted so that it is not deleted under callers nose. It is the
 * callers responsibilty to release this refcount when the item is inserted
 * into the hash + lru q or freed.
 */
static struct item *
_item_alloc_config(uint8_t id, const char *key, uint8_t nkey, uint32_t dataflags, rel_time_t
		exptime, uint32_t nbyte, bool lock_slab, bool reserved_item, int32_t config_num)
{
	struct item *it;  /* item */
	struct item *uit; /* unexpired lru item */

	ASSERT(id >= SLABCLASS_MIN_ID && id <= SLABCLASS_MAX_ID);

	/*
	 * We try to obtain an item in the following order:
	 *  1)  by acquiring an expired item;
	 *  2)  by getting a free slot from the last slab in current class;
	 *  3)  by evicting a slab, if slab eviction(s) are enabled;
	 *  4)  by evicting an item, if item lru eviction is enabled.
	 */
	if(reserved_item) {
		it = item_get_from_lruq(id, reserved_item_lruq); /* expired / unexpired lru item */
	} else {
		it = item_get_from_lruq(id, item_lruq); /* expired / unexpired lru item */
	}

	if (it == NULL) {
		if (reserved_item)
			log_debug(LOG_VERB, "cannot get item '%.*s' from LRU reserveditem=true", nkey, key);
		else
			log_debug(LOG_VERB, "cannot get item '%.*s' from LRU reserveditem=false", nkey, key);
	}

	if (it != NULL && item_expired(it)) {
		/* 1) this is an expired item, always use it */
		stats_slab_incr(id, item_expire);
		stats_slab_settime(id, item_expire_ts, it->exptime);

		/* Count expired leases */
		if(item_has_i_lease(it)) {
			stats_thread_incr(expired_leases);
			stats_thread_incr(expired_i_leases);
		} else if (item_has_q_inv_lease(it) || item_has_q_ref_lease(it) || item_has_q_incr_lease(it)) {
			stats_thread_incr(expired_leases);
			stats_thread_incr(expired_q_leases);
		} else if (item_has_co_lease(it)) {
			stats_thread_incr(expired_leases);
			if (item_has_c_lease(it))
				stats_thread_incr(expired_c_leases);
			else if (item_has_o_lease(it))
				stats_thread_incr(expired_o_leases);
		}

		item_reuse(it);
		goto done;
	}

	uit = (settings.evict_opt & EVICT_LRU)? it : NULL; /* keep if can be used */

	if (reserved_item) {
		it = slab_get_reserved_item(id, lock_slab);
	} else {
		it = slab_get_item(id);
	}
	if (it != NULL) {
		/* 2) or 3) either we allow random eviction a free item is found */
		goto done;
	}

	if (uit != NULL) {
		/* 4) this is an lru item and we can reuse it */
		it = uit;
		stats_slab_incr(id, item_evict);
		stats_slab_settime(id, item_evict_ts, time_now());

		item_reuse(it);
		goto done;
	}

	// finally, try evict slab if we cannot find any item in EVICT_LRU option
	if (settings.evict_opt == 0) {
		return NULL;
	}

	if (reserved_item == false)
		it = slab_get_item_by_evict_slab(id);
	else
		it = slab_get_item_by_evict_reserved_slab(id);

	if (it != NULL)
		goto done;

	if (reserved_item == true)
		log_warn("server error on allocating item in slab %"PRIu8" key '%.*s', reserveditem=true", id, nkey, key, reserved_item);

	if (reserved_item == false)
		log_warn("server error on allocating item in slab %"PRIu8" key '%.*s', reserveditem=false", id, nkey, key, reserved_item);

	stats_thread_incr(server_error);

	return NULL;

	done:
	ASSERT(it->id == id);
	ASSERT(!item_is_linked(it));
	ASSERT(!item_is_slabbed(it));
	ASSERT(it->offset != 0);
	ASSERT(it->refcount == 0);

	item_acquire_refcount(it);

	it->flags = settings.use_cas ? ITEM_CAS : 0;
	it->dataflags = dataflags;
	it->nbyte = nbyte;
	it->exptime = exptime;
	it->nkey = nkey;
	it->sess_status = ALIVE;
	it->coflags = 0;
	it->p = 0;
	it->config_number = config_num;
//	printf("XXXXX%s-%dXXXXX\n", key, config_num);

#if defined MC_MEM_SCRUB && MC_MEM_SCRUB == 1
	memset(it->end, 0xff, slab_item_size(it->id) - ITEM_HDR_SIZE);
#endif
	memcpy(item_key(it), key, nkey);

	stats_slab_incr(id, item_acquire);

	log_debug(LOG_VERB, "alloc it '%.*s' at offset %"PRIu32" with id %"PRIu8
			" expiry %u refcount %"PRIu16"", it->nkey, item_key(it),
			it->offset, it->id, it->exptime, it->refcount);

	return it;
}

static struct item *
_item_alloc(uint8_t id, const char *key, uint8_t nkey, uint32_t dataflags,
		rel_time_t exptime, uint32_t nbyte, bool lock_slab, bool reserved_item) {
	return _item_alloc_config(id, key, nkey, dataflags, exptime, nbyte,
			lock_slab, reserved_item, -1);
}


struct item *
item_alloc(uint8_t id, char *key, uint8_t nkey, uint32_t dataflags,
		rel_time_t exptime, uint32_t nbyte)
{
	struct item *it;

	pthread_mutex_lock(&cache_lock);
	it = _item_alloc(id, key, nkey, dataflags, exptime, nbyte, true, false);
	pthread_mutex_unlock(&cache_lock);

	return it;
}

struct item *
item_alloc_config(uint8_t id, char *key, uint8_t nkey, uint32_t dataflags,
		rel_time_t exptime, uint32_t nbyte, int32_t config_num)
{
	struct item *it;
	pthread_mutex_lock(&cache_lock);
	it = _item_alloc_config(id, key, nkey, dataflags, exptime, nbyte, true, false, config_num);
	pthread_mutex_unlock(&cache_lock);

	return it;
}

static void
item_free(struct item *it, bool lock_slab)
{
	ASSERT(it->magic == ITEM_MAGIC);
	if(item_is_lease_holder(it) || item_has_co_lease(it) || item_is_sess(it)) {
		slab_put_reserved_item(it, lock_slab);
	} else {
		slab_put_item(it);
	}

	if(item_has_q_lease(it)) {
		stats_thread_decr(active_q_lease);
	} else if (item_has_c_lease(it)) {
		stats_thread_decr(active_c_lease);
	} else if (item_has_o_lease(it)) {
		stats_thread_decr(active_o_lease);
	}

	stats_thread_incr(items_free);
}

/*
 * Link an item into the hash table and lru q
 */
static void
_item_link(struct item *it)
{
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(!item_is_linked(it));
	ASSERT(!item_is_slabbed(it));

	log_debug(LOG_DEBUG, "link it '%.*s' at offset %"PRIu32" with flags "
			"%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
			it->flags, it->id);

	it->flags |= ITEM_LINKED;
	item_set_cas(it, item_next_cas());

	assoc_insert(it);
	item_link_q(it, true);
}

/*
 * Unlinks an item from the lru q and hash table. Free an unlinked
 * item if it's refcount is zero.
 */
static void
_item_unlink(struct item *it)
{
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(item_is_linked(it));

	if (item_is_lease_holder(it))
		item_unset_pinned(it);
	else if (item_is_co_lease_holder(it))
		item_unset_pinned(it);
	else if (item_is_ptrans(it))
		item_unset_pinned(it);
	else if (item_is_hotkeys(it))
		item_unset_pinned(it);

	log_debug(LOG_DEBUG, "unlink it '%.*s' at offset %"PRIu32" with flags "
			"%02x id %"PRId8" refcount %"PRIu16"", it->nkey, item_key(it), it->offset,
			it->flags, it->id, it->refcount);

	if (item_is_linked(it)) {
		it->flags &= ~ITEM_LINKED;

		assoc_delete(item_key(it), it->nkey);

		item_unlink_q(it);

		if (it->refcount == 0) {
			item_free(it, true);
		}

		stats_thread_incr(items_unlink);
	}
}




/*
 * Decrement the refcount on an item. Free an unliked item if its refcount
 * drops to zero.
 */
static void
_item_remove2(struct item *it, bool lock_slab)
{
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(!item_is_slabbed(it));

	log_debug(LOG_DEBUG, "remove it '%.*s' at offset %"PRIu32" with flags "
			"%02x id %"PRId8" refcount %"PRIu16"", it->nkey, item_key(it),
			it->offset, it->flags, it->id, it->refcount);

	if (it->refcount != 0) {
		item_release_refcount(it);
	}

	if (it->refcount == 0 && !item_is_linked(it)) {
		item_free(it, lock_slab);
	}
}


static void
_item_remove(struct item *it)
{
	_item_remove2(it, true);
}

/*
 * Decrement the refcount on an item. Free an unliked item if its refcount
 * drops to zero.
 */
void
item_remove(struct item *it)
{
	pthread_mutex_lock(&cache_lock);
	_item_remove(it);
	pthread_mutex_unlock(&cache_lock);
}

//static void
//_item_unlink_by_key(const char *key, size_t nkey) {
//	struct item* it = assoc_find(key, nkey);
//
//	if (it != NULL)
//		_item_unlink(it);
//}

/*
 * Unlink an item and remove it (if its recount drops to zero).
 */
void
item_delete(struct item *it)
{
	pthread_mutex_lock(&cache_lock);

	_item_unlink(it);
	_item_remove(it);

	pthread_mutex_unlock(&cache_lock);
}

/*
 * Touch the item by moving it to the tail of lru q only if it wasn't
 * touched ITEM_UPDATE_INTERVAL secs back.
 */
static void
_item_touch(struct item *it)
{
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(!item_is_slabbed(it));

	if (it->atime >= (time_now() - ITEM_UPDATE_INTERVAL)) {
		return;
	}

	log_debug(LOG_VERB, "update it '%.*s' at offset %"PRIu32" with flags "
			"%02x id %"PRId8"", it->nkey, item_key(it), it->offset,
			it->flags, it->id);

	if (item_is_linked(it)) {
		item_unlink_q(it);
		item_link_q(it, false);
	}
}

void
item_touch(struct item *it)
{
	if (it->atime >= (time_now() - ITEM_UPDATE_INTERVAL)) {
		return;
	}

	pthread_mutex_lock(&cache_lock);
	_item_touch(it);
	pthread_mutex_unlock(&cache_lock);
}

/*
 * Replace one item with another in the hash table and lru q.
 */
static void
_item_replace(struct item *it, struct item *nit)
{
	ASSERT(it->magic == ITEM_MAGIC);
	ASSERT(!item_is_slabbed(it));

	ASSERT(nit->magic == ITEM_MAGIC);
	ASSERT(!item_is_slabbed(nit));

	log_debug(LOG_VERB, "replace it '%.*s' at offset %"PRIu32" id %"PRIu8" "
			"with one at offset %"PRIu32" id %"PRIu8"", it->nkey,
			item_key(it), it->offset, it->id, nit->offset, nit->id);

	_item_unlink(it);
	_item_link(nit);
}

// TODO: make this work with the reserved lruq as well.
static char *
_item_cache_dump(uint8_t id, uint32_t limit, uint32_t *bytes)
{
	const size_t memlimit = ITEM_CACHEDUMP_MEMLIMIT;
	char *buffer;
	uint32_t bufcurr;
	struct item *it;
	uint32_t len;
	uint32_t shown = 0;
	char key_temp[KEY_MAX_LEN + 1];
	char temp[KEY_MAX_LEN * 2];

	buffer = mc_alloc(memlimit);
	if (buffer == NULL) {
		return NULL;
	}

	for (bufcurr = 0, it = TAILQ_FIRST(&item_lruq[id]);
			it != NULL && (limit == 0 || shown < limit);
			it = TAILQ_NEXT(it, i_tqe)) {

		ASSERT(it->nkey <= KEY_MAX_LEN);
		/* copy the key since it may not be null-terminated in the struct */
		strncpy(key_temp, item_key(it), it->nkey);
		key_temp[it->nkey] = '\0'; /* terminate */
		len = snprintf(temp, sizeof(temp), "ITEM %s [%d b; %lu s]\r\n",
				key_temp, it->nbyte,
				(unsigned long)it->exptime + time_started());
		if (len >= sizeof(temp)) {
			log_debug(LOG_WARN, "item log was truncated during cache dump");
		}
		if (bufcurr + len + (sizeof("END\r\n") - 1) > memlimit) {
			break;
		}
		memcpy(buffer + bufcurr, temp, len);
		bufcurr += len;
		shown++;
	}

	memcpy(buffer + bufcurr, "END\r\n", sizeof("END\r\n") - 1);
	bufcurr += sizeof("END\r\n") - 1;

	*bytes = bufcurr;

	return buffer;
}

char *
item_cache_dump(uint8_t id, uint32_t limit, uint32_t *bytes)
{
	char *ret;

	pthread_mutex_lock(&cache_lock);
	ret = _item_cache_dump(id, limit, bytes);
	pthread_mutex_unlock(&cache_lock);

	return ret;
}

/*
 * Return an item if it hasn't been marked as expired, lazily expiring
 * item as-and-when needed
 *
 * When a non-null item is returned, it's the callers responsibily to
 * release refcount on the item
 */
static struct item *
_item_get(const char *key, size_t nkey)
{
	struct item *it;

	it = assoc_find(key, nkey);
	if (it == NULL) {
		log_debug(LOG_VERB, "get it '%.*s' not found", nkey, key);
		return NULL;
	}

	log_debug(LOG_VERB, "get it Time: %d; Exptime: %d",
			time_now(), it->exptime);

	if (it->exptime != 0 && it->exptime <= time_now()) {
		/* Count expired leases */
		if(item_has_i_lease(it)) {
			stats_thread_incr(expired_leases);
			stats_thread_incr(expired_i_leases);
		} else if (item_has_q_inv_lease(it) || item_has_q_ref_lease(it) || item_has_q_incr_lease(it)) {
			stats_thread_incr(expired_leases);
			stats_thread_incr(expired_q_leases);
		} else if (item_has_co_lease(it)) {
			stats_thread_incr(expired_leases);
			if (item_has_c_lease(it))
				stats_thread_incr(expired_c_leases);
			else if (item_has_o_lease(it))
				stats_thread_incr(expired_o_leases);
		}

		_item_unlink(it);
		stats_slab_incr(it->id, item_expire);
		stats_slab_settime(it->id, item_reclaim_ts, time_now());
		stats_slab_settime(it->id, item_expire_ts, it->exptime);
		log_debug(LOG_VERB, "get it '%.*s' expired and nuked", nkey, key);
		return NULL;
	}

	if (settings.oldest_live != 0 && settings.oldest_live <= time_now() &&
			it->atime <= settings.oldest_live) {
		_item_unlink(it);
		stats_slab_incr(it->id, item_evict);
		stats_slab_settime(it->id, item_evict_ts, time_now() );
		log_debug(LOG_VERB, "it '%.*s' nuked", nkey, key);
		return NULL;
	}

	item_acquire_refcount(it);

	log_debug(LOG_VERB, "get it '%.*s' found at offset %"PRIu32" with flags "
			"%02x id %"PRIu8" refcount %"PRIu32"", it->nkey, item_key(it),
			it->offset, it->flags, it->id, it->refcount);

	return it;
}

/* Allocate an item with value size 0 that will act as the lease holder */
static struct item*
_item_create_reserved_item(const char* key, uint8_t nkey, uint32_t vlen, bool lock_slab)
{
	uint32_t flags = 0;
	time_t exptime = settings.lease_token_expiry / 1000;
	struct item *it;
	uint8_t id;

	if(exptime == 0) {
		/* Minimum 1 second expiry time */
		exptime = 1;
	}

	if(exptime > 0) {
		/* Add 1 second to account for rounding errors */
		exptime++;
	}

	id = item_slabid(nkey, vlen);
	it = _item_alloc(id, key, nkey, flags, time_reltime(exptime), vlen, lock_slab, true);
	if (it != NULL) {
		item_set_pinned(it);
	}

	return it;
}

struct item*
_item_get_pending_version(char* key, size_t nkey) {
	size_t pv_nkey = nkey + PREFIX_KEY_LEN;
	char pv_key[pv_nkey];
	struct item* pv_it;

	mc_get_version_key(key, nkey, &pv_key);

	pv_it = _item_get(pv_key, pv_nkey);

	return pv_it;
}

struct item*
_item_create_pending_version(struct item*it) {
	time_t exptime = settings.lease_token_expiry / 1000;
	struct item* pv_it = NULL;
	uint8_t id;
	size_t pv_nkey = it->nkey + PREFIX_KEY_LEN;
	char pv_key[pv_nkey];

	mc_get_version_key(item_key(it), it->nkey, &pv_key);

	if(exptime == 0) {
		/* Minimum 1 second expiry time */
		exptime = 1;
	}

	if(exptime > 0) {
		/* Add 1 second to account for rounding errors */
		exptime++;
	}

	id = item_slabid(pv_nkey, it->nbyte);
	pv_it = _item_alloc(id, pv_key, pv_nkey, it->dataflags, time_reltime(exptime),
			it->nbyte, true, false);

	return pv_it;
}

struct item*
_item_get_lease(char* key, size_t nkey) {
	size_t lease_nkey = nkey + PREFIX_KEY_LEN;
	char lease_key[lease_nkey];

	mc_get_lease_key(key, nkey, &lease_key);

	return _item_get(lease_key, lease_nkey);
}

struct item*
_item_get_pending(char* key, size_t nkey) {
	size_t pending_nkey = nkey + PREFIX_KEY_LEN;
	char pending_key[pending_nkey];

	mc_get_pending_key(key, nkey, &pending_key);

	return _item_get(pending_key, pending_nkey);
}

struct item*
_item_get_ptrans(char* key, size_t nkey) {
	size_t ptrans_nkey = nkey + PREFIX_KEY_LEN;
	char ptrans_key[ptrans_nkey];
	mc_get_ptrans_key(key, nkey, &ptrans_key);
	return _item_get(ptrans_key, ptrans_nkey);
}

struct item*
_item_create_lease(char* key, size_t nkey, lease_token_t* token) {
	time_t exptime = settings.lease_token_expiry / 1000;
	struct item* lease_it = NULL;
	uint8_t id;
	size_t lease_nkey = nkey + PREFIX_KEY_LEN;
	char lease_key[lease_nkey];
	int res;
	char buf[INCR_MAX_STORAGE_LEN];

	mc_get_lease_key(key, nkey, &lease_key);

	*token = 0;

	if(exptime == 0) {
		/* Minimum 1 second expiry time */
		exptime = 1;
	}

	if(exptime > 0) {
		/* Add 1 second to account for rounding errors */
		exptime++;
	}

	*token = lease_next_token();
	res = snprintf(buf, INCR_MAX_STORAGE_LEN, "%"PRIu64, *token);
	id = item_slabid(lease_nkey, res);

	lease_it = _item_alloc(id, lease_key, lease_nkey, 0,
			time_reltime(exptime), res, true, true);

	if (lease_it != NULL) {
		memcpy(item_data(lease_it), buf, res);
		item_set_pinned(lease_it);
	}

	return lease_it;
}

struct item*
_item_create_ptrans(char* key, size_t nkey, int64_t token) {
	time_t exptime = settings.lease_token_expiry / 1000;
	struct item* ptrans_it = NULL;
	uint8_t id;
	size_t ptrans_nkey = nkey + PREFIX_KEY_LEN;
	char ptrans_key[ptrans_nkey];
	int res;
	char buf[INCR_MAX_STORAGE_LEN];

	mc_get_ptrans_key(key, nkey, &ptrans_key);

	if(exptime == 0) {
		/* Minimum 1 second expiry time */
		exptime = 1;
	}

	if(exptime > 0) {
		/* Add 1 second to account for rounding errors */
		exptime++;
	}

	res = snprintf(buf, INCR_MAX_STORAGE_LEN, "%"PRIu64, token);
	id = item_slabid(ptrans_nkey, res);

	ptrans_it = _item_alloc(id, ptrans_key, ptrans_nkey, 0, time_reltime(exptime),
			res, true, true);

	if (ptrans_it != NULL) {
		memcpy(item_data(ptrans_it), buf, res);
		item_set_pinned(ptrans_it);
	}

	return ptrans_it;
}

struct item*
_item_get_co_lease(char* key, size_t nkey) {
	size_t lease_nkey = nkey + PREFIX_KEY_LEN;
	char lease_key[lease_nkey];
	mc_get_co_lease_key(key, nkey, &lease_key);
	return _item_get(lease_key, lease_nkey);
}

struct item *
item_get(const char *key, size_t nkey)
{
	struct item *it;

	pthread_mutex_lock(&cache_lock);
	it = _item_get(key, nkey);
	pthread_mutex_unlock(&cache_lock);

	return it;
}

/*
 * Flushes expired items after a "flush_all" call. Expires items that
 * are more recent than the oldest_live setting
 *
 * TODO: make this work with reserved lruq as well
 */
static void
_item_flush_expired(void)
{
	uint8_t i;
	struct item *it, *next;

	if (settings.oldest_live == 0) {
		return;
	}

	for (i = SLABCLASS_MIN_ID; i <= SLABCLASS_MAX_ID; i++) {
		/*
		 * Lru q is sorted in ascending time order -- oldest to most recent.
		 * Since time is computed at granularity of one sec, we would like
		 * to flush items which are accessed during this one sec. So we
		 * proactively flush most recent ones in a given queue until we hit
		 * an item older than oldest_live. Older items in this queue are then
		 * lazily expired by oldest_live check in item_get.
		 */
		TAILQ_FOREACH_REVERSE_SAFE(it, &item_lruq[i], item_tqh, i_tqe, next) {
			ASSERT(!item_is_slabbed(it));

			if (it->atime < settings.oldest_live) {
				break;
			}

			_item_unlink(it);

			stats_slab_incr(it->id, item_evict);
			stats_slab_settime(it->id, item_evict_ts, time_now());
		}
	}
}

void
item_flush_expired(void)
{
	pthread_mutex_lock(&cache_lock);
	_item_flush_expired();
	pthread_mutex_unlock(&cache_lock);
}

/*
 * Store an item in the cache according to the semantics of one of the
 * update commands - {set, add, replace, append, prepend, cas}
 */
static item_store_result_t
_item_store(struct item *it, req_type_t type, struct conn *c, bool lock_slab)
{
	item_store_result_t result;  /* item store result */
	bool store_it;               /* store item ? */
	char *key;                   /* item key */
	struct item *oit, *nit;      /* old (existing) item & new item */
	uint8_t id;                  /* slab id */
	uint32_t total_nbyte;
	struct item *lease_it = NULL;
	lease_token_t lease_token = 0;

	result = NOT_STORED;

	// if new item has no value, not store
	if (it->nbyte == 0 && !item_is_lease_holder(it) && !item_has_co_lease(it))
		return result;

	store_it = true;

	key = item_key(it);
	nit = NULL;
	oit = _item_get(key, it->nkey);
	if (oit == NULL) {
		switch (type) {
		case REQ_IQSET:
			stats_thread_incr(iqset);

			lease_it = _item_get_lease(key, it->nkey);

			if (lease_it == NULL || !item_has_i_lease(lease_it)) {
				stats_thread_incr(iqset_miss);
				store_it = false;
			} else {
				lease_token = _item_lease_value(lease_it);

				if (c->lease_token != lease_token) {
					stats_thread_incr(iqset_miss);
					store_it = false;
					stats_thread_incr(set_miss_token);
					log_debug(LOG_VERB, "iqset miss: new token %"PRIu64" old token %"PRIu64, c->lease_token, lease_token);
				} else {
					// remove lease token
					_item_unlink(lease_it);

					if (it->nbyte == 0) {
						store_it = false;
						log_debug(LOG_VERB, "iqset item has no value");
						break;
					} else {
						struct item* pending_it = _item_get_pending(key, it->nkey);
						if (pending_it != NULL) {
							it->p = 1;
							_item_unlink(pending_it);
							_item_remove(pending_it);
						}
					}

					stats_slab_incr(it->id, set_success);
					stats_thread_incr(released_i_lease);
				}
			}

			break;

		case REQ_SET:
			stats_slab_incr(it->id, set_success);
			break;

		case REQ_ADD:
			stats_slab_incr(it->id, add_success);
			break;

		case REQ_REPLACE:
			stats_thread_incr(replace_miss);
			store_it = false;
			break;

		case REQ_APPEND:
			stats_thread_incr(append_miss);
			store_it = false;
			break;

		case REQ_PREPEND:
			stats_thread_incr(prepend_miss);
			store_it = false;
			break;

		case REQ_CAS:
			stats_thread_incr(cas_miss);
			result = NOT_FOUND;
			store_it = false;
			break;

		case REQ_IQGET:
		case REQ_DELETE:
		case REQ_QAREG:
		case REQ_QAREAD:
			store_it = true;
			break;
		default:
			NOT_REACHED();
		}
	} else {
		switch (type) {
		case REQ_IQSET:
			stats_thread_incr(iqset);

			lease_it = _item_get_lease(key, it->nkey);

			if (lease_it == NULL || !item_has_i_lease(lease_it)) {
				stats_thread_incr(iqset_miss);
				store_it = false;
			} else {
				lease_token = _item_lease_value(lease_it);

				if (c->lease_token != lease_token) {
					stats_thread_incr(iqset_miss);
					store_it = false;
					stats_thread_incr(set_miss_token);
					log_debug(LOG_VERB, "iqset miss: new token %"PRIu64" old token %"PRIu64, c->lease_token, lease_token);
				} else {
					// remove lease token
					_item_unlink(lease_it);

					if (it->nbyte == 0) {
						store_it = false;
						log_debug(LOG_VERB, "iqset item has no value");
						break;
					} else {
						struct item* pending_it = _item_get_pending(key, it->nkey);
						if (pending_it != NULL) {
							it->p = 1;
							_item_unlink(pending_it);
							_item_remove(pending_it);
						}
					}

					stats_slab_incr(it->id, set_success);
					stats_thread_incr(released_i_lease);
				}
			}
			break;

		case REQ_SET:
			stats_slab_incr(it->id, set_success);
			break;

		case REQ_ADD:
			if (oit->nbyte == 0) {
				store_it = true;
				stats_slab_incr(oit->id, add_success);
				break;
			}

			stats_thread_incr(add_exist);
			/*
			 * Add only adds a non existing item. However we promote the
			 * existing item to head of the lru q
			 */
			_item_touch(oit);
			store_it = false;
			break;

		case REQ_REPLACE:
			if (oit->nbyte == 0) {
				store_it = false;
			} else {
				stats_slab_incr(oit->id, replace_hit);
				stats_slab_incr(it->id, replace_success);
			}
			break;

		case REQ_APPEND:
			stats_slab_incr(oit->id, append_hit);

			total_nbyte = oit->nbyte + it->nbyte;
			id = item_slabid(oit->nkey, total_nbyte);
			if (id == SLABCLASS_INVALID_ID) {
				/* FIXME: logging client error but not sending CLIENT ERROR
				 * to the client because we are inside the item module, which
				 * technically shouldn't directly handle commands. There is not
				 * a proper return status to indicate such an error.
				 * This can only be fixed by moving the command-aware logic
				 * into a separate module.
				 */
				log_debug(LOG_NOTICE, "client error on c %d for req of type %d"
						" with key size %"PRIu8" and value size %"PRIu32,
						c->sd, c->req_type, oit->nkey, total_nbyte);

				stats_thread_incr(cmd_error);
				store_it = false;
				break;
			}

			/* if oit is large enough to hold the extra data and left-aligned,
			 * which is the default behavior, we copy the delta to the end of
			 * the existing data. Otherwise, allocate a new item and store the
			 * payload left-aligned.
			 */

			// nbyte = 0 means that item does not exist
			if (oit->nbyte == 0) {
				store_it = false;
				break;
			}

			nit = _item_alloc(id, key, oit->nkey, oit->dataflags,
					oit->exptime, total_nbyte, true, false);

			if (nit == NULL) {
				store_it = false;
				break;
			}

			memcpy(item_data(nit), item_data(oit), oit->nbyte);
			memcpy(item_data(nit) + oit->nbyte, item_data(it), it->nbyte);
			it = nit;

			store_it = true;
			stats_slab_incr(it->id, append_success);
			break;

		case REQ_PREPEND:
			stats_slab_incr(oit->id, prepend_hit);

			/*
			 * Alloc new item - nit to hold both it and oit
			 */
			total_nbyte = oit->nbyte + it->nbyte;
			id = item_slabid(oit->nkey, total_nbyte);
			if (id == SLABCLASS_INVALID_ID) {
				log_debug(LOG_NOTICE, "client error on c %d for req of type %d"
						" with key size %"PRIu8" and value size %"PRIu32,
						c->sd, c->req_type, oit->nkey, total_nbyte);

				stats_thread_incr(cmd_error);
				store_it = false;
				break;
			}
			/* if oit is large enough to hold the extra data and is already
			 * right-aligned, we copy the delta to the front of the existing
			 * data. Otherwise, allocate a new item and store the payload
			 * right-aligned, assuming more prepends will happen in the future.
			 */

			// nbyte = 0 means that item does not exist
			if (oit->nbyte == 0) {
				store_it = false;
				break;
			}

			nit = _item_alloc(id, key, oit->nkey, oit->dataflags,
					oit->exptime, total_nbyte, true, false);
			if (nit == NULL) {
				store_it = false;
				break;
			}

			nit->flags |= ITEM_RALIGN;
			memcpy(item_data(nit), item_data(it), it->nbyte);
			memcpy(item_data(nit) + it->nbyte, item_data(oit), oit->nbyte);

			it = nit;

			store_it = true;
			stats_slab_incr(it->id, prepend_success);
			break;

		case REQ_CAS:
			if (item_cas(it) != item_cas(oit)) {
				log_debug(LOG_DEBUG, "cas mismatch %"PRIu64" != %"PRIu64 "on "
						"it '%.*s'", item_cas(oit), item_cas(it), it->nkey,
						item_key(it));
				stats_slab_incr(oit->id, cas_badval);
				result = EXISTS;
				break;
			}

			stats_slab_incr(oit->id, cas_hit);
			stats_slab_incr(it->id, cas_success);
			break;

		case REQ_DELETE:
			// Lease holder replacing an existing entry
			store_it = true;
			break;

		case REQ_QAREG:
			store_it = true;
			break;

		case REQ_QAREAD:
			store_it = true;
			break;
		case REQ_IQGET:
			store_it = false;
			break;

		default:
			NOT_REACHED();
		}
	}

	if (result == NOT_STORED && store_it) {
		if (oit != NULL) {
			_item_replace(oit, it);
		} else {
			_item_link(it);
		}
		result = STORED;

		log_debug(LOG_VERB, "store it '%.*s'at offset %"PRIu32" with flags %02x"
				" id %"PRId8"", it->nkey, item_key(it), it->offset, it->flags,
				it->id);
	} else {
		log_debug(LOG_VERB, "did not store it '%.*s'at offset %"PRIu32" with flags %02x"
				" id %"PRId8"", it->nkey, item_key(it), it->offset, it->flags,
				it->id);
	}

	/* release our reference, if any */
	if (oit != NULL) {
		_item_remove2(oit, lock_slab);
	}

	if (nit != NULL) {
		_item_remove(nit);
	}

	if (lease_it != NULL) {
		_item_remove(lease_it);
	}

	return result;
}

item_store_result_t
item_store(struct item *it, req_type_t type, struct conn *c)
{
	item_store_result_t ret = NOT_STORED;

	pthread_mutex_lock(&cache_lock);
	ret = _item_store(it, type, c, true);
	pthread_mutex_unlock(&cache_lock);

	return ret;
}

lease_token_t _item_lease_value(struct item* it) {
	uint64_t value;
	char *ptr;

	if (it == NULL) {
		return 0;
	}

	ptr = item_data(it);

	if (!mc_strtoull_len(ptr, &value, it->nbyte)) {
		return -1;
	}

	return (lease_token_t)value;
}

/*
 * Add a delta value (positive or negative) to an item.
 */
static item_delta_result_t
_item_add_delta(struct conn *c, char *key, size_t nkey, bool incr,
		int64_t delta, char *buf)
{
	int res;
	char *ptr;
	uint64_t value;
	struct item *it;

	it = _item_get(key, nkey);
	if (it == NULL) {
		return DELTA_NOT_FOUND;
	}

	ptr = item_data(it);

	if (!mc_strtoull_len(ptr, &value, it->nbyte)) {
		_item_remove(it);
		return DELTA_NON_NUMERIC;
	}

	if (incr) {
		value += delta;
	} else if (delta > value) {
		value = 0;
	} else {
		value -= delta;
	}

	if (incr) {
		stats_slab_incr(it->id, incr_hit);
	} else {
		stats_slab_incr(it->id, decr_hit);
	}

	res = mc_snprintf(buf, INCR_MAX_STORAGE_LEN, "%"PRIu64, value);
	ASSERT(res < INCR_MAX_STORAGE_LEN);
	if (res > it->nbyte) { /* need to realloc */
		struct item *new_it;
		uint8_t id;

		id = item_slabid(it->nkey, res);
		if (id == SLABCLASS_INVALID_ID) {
			log_debug(LOG_NOTICE, "client error on c %d for req of type %d"
					" with key size %"PRIu8" and value size %"PRIu32, c->sd,
					c->req_type, it->nkey, res);
		}

		new_it = _item_alloc(id, item_key(it), it->nkey, it->dataflags,
				it->exptime, res, true, false);
		if (new_it == NULL) {
			_item_remove(it);
			return DELTA_EOM;
		}
		if (incr) {
			stats_slab_incr(new_it->id, incr_success);
		} else {
			stats_slab_incr(new_it->id, decr_success);
		}

		memcpy(item_data(new_it), buf, res);
		new_it->p = it->p;
		_item_replace(it, new_it);
		_item_remove(new_it);
	} else {
		/*
		 * Replace in-place - when changing the value without replacing
		 * the item, we need to update the CAS on the existing item
		 */
		if (incr) {
			stats_slab_incr(it->id, incr_success);
		} else {
			stats_slab_incr(it->id, decr_success);
		}

		item_set_cas(it, item_next_cas());
		memcpy(item_data(it), buf, res);
		it->nbyte = res;
	}

	_item_remove(it);

	return DELTA_OK;
}

item_delta_result_t
item_add_delta(struct conn *c, char *key, size_t nkey, int incr,
		int64_t delta, char *buf)
{
	item_delta_result_t ret;

	pthread_mutex_lock(&cache_lock);
	ret = _item_add_delta(c, key, nkey, incr, delta, buf);
	pthread_mutex_unlock(&cache_lock);

	return ret;
}

/* Remove the lease token rather than deleting the item.
 * Used in qaread session
 */
rstatus_t
item_get_and_unlease(char* key, uint8_t nkey, lease_token_t token_val, struct conn *c) {
	rstatus_t status = MC_OK;
	struct item* lease_it = NULL;

	pthread_mutex_lock(&cache_lock);

	log_debug(LOG_VERB, "get_and_unlease for '%.*s'", nkey, key);

	lease_it = _item_get_lease(key, nkey);

	if (lease_it != NULL && _item_lease_value(lease_it) == token_val) {
		ASSERT(!item_has_q_inv_lease(lease_it));
		ASSERT(!item_has_q_incr_lease(lease_it));

		_item_unlink(lease_it);
		_item_remove(lease_it);

		stats_thread_incr(unlease);

		if (item_has_i_lease(lease_it)) {
			stats_thread_incr(released_i_lease);
		}
	} else {
		if (lease_it != NULL)
			_item_remove(lease_it);
	}

	pthread_mutex_unlock(&cache_lock);

	return status;
}

/**
 * Get I lease for a key. If it fails because someone is holding lease for this key,
 * markedVal is set to 0. Otherwise, markedVal = 1
 */
rstatus_t
item_quarantine_and_register(char* tid, size_t ntid, char* key,
		size_t nkey, u_int8_t *markedVal,struct conn *c) {
	struct item *trans_it = NULL;
	struct item* lease_it = NULL;
	struct item* it = NULL;

	log_debug(LOG_VERB, "quarantine_and_register for '%.*s'", nkey, key);

	pthread_mutex_lock(&cache_lock);

	trans_it = _item_get(tid, ntid); 		// get transaction
	lease_it = _item_get_lease(key, nkey);	// get lease item

	if (lease_it != NULL && item_has_i_lease(lease_it)) {
		_item_unlink(lease_it);
		_item_remove(lease_it);
		lease_it = NULL;
	}

	_item_assoc_tid_lease(c, lease_it, key, nkey, tid, ntid);
	_item_assoc_key_tid(c, trans_it, key, nkey, tid, ntid);

	// at this time, transaction item for this session should be ready
	// now it's time to reason about the lease item
	// no matter lease token exists or not, create a new one and overwrite the old one
	if (lease_it != NULL) {
		_item_remove(lease_it);

		// this lease item will be replaced
		stats_thread_incr(qlease_voids);
	}

	// update the lease time for item
	it = _item_get(key, nkey);
	lease_it = _item_get_lease(key, nkey);
	if (it != NULL) {
		if (lease_it != NULL)
			it->exptime = lease_it->exptime;
		_item_remove(it);
	}

	if (lease_it != NULL)
		_item_remove(lease_it);

	if (trans_it != NULL) {
		_item_remove(trans_it);
	}

	pthread_mutex_unlock(&cache_lock);

	*markedVal = 1;
	return MC_OK;
}

item_co_result_t
item_ciget(char* sid, size_t nsid, char *key, size_t nkey, lease_token_t lease_token, struct conn *c, struct item** item, lease_token_t* new_lease_token) {
	struct item* sess_it = NULL;
	struct item* colease_it = NULL;
	struct item* iqlease_it = NULL;
	struct item* it = NULL;

	rstatus_t status = CO_OK;

	pthread_mutex_lock(&cache_lock);

	stats_thread_incr(ciget);

	log_debug(LOG_VERB, "ciget for sess '%.*s', key '%.*s'", nsid, sid, nkey, key);

	// get session entry from ActiveSessions
	sess_it = _item_get(sid, nsid);

	// session already aborted
	// clean the session from ActiveSessions
	if (sess_it != NULL && sess_it->sess_status == ABORT) {
		_item_unlink(sess_it);
		item_unset_pinned(sess_it);
		_item_remove(sess_it);
		stats_thread_incr(sess_abort);
		pthread_mutex_unlock(&cache_lock);

		return CO_ABORT;
	}

	if (sess_it != NULL)
		_item_remove(sess_it);

	// check the O lease status
	colease_it = _item_get_co_lease(key, nkey);
	if (colease_it != NULL && item_has_o_lease(colease_it)) {
		if (trig_check_keylist(item_data(colease_it), colease_it->nbyte, sid, nsid) == TRIG_OK) {
			it = _item_get_pending_version(key, nkey);
			if (it != NULL) {
				*item = it;
				stats_thread_incr(get_hit);
			}

			status = CO_OK;
			_item_remove(colease_it);
			pthread_mutex_unlock(&cache_lock);

			return status;
		} else {
			status = CO_ABORT;
			_item_remove(colease_it);
			clean_session(sid, nsid, c);
			pthread_mutex_unlock(&cache_lock);

			return status;
		}
	}

	// check whether the key holds an I or Q lease
	iqlease_it = _item_get_lease(key, nkey);
	if (iqlease_it != NULL) {
		if (item_has_i_lease(iqlease_it)) {
			if (_item_lease_value(iqlease_it) == lease_token) {	// same trans
				status = CO_OK;
			} else {	// different trans
				status = CO_RETRY;
			}

			_item_remove(iqlease_it);
			if (colease_it != NULL) {
				_item_remove(colease_it);
			}
			pthread_mutex_unlock(&cache_lock);
			return status;
		} else if (item_has_q_lease(iqlease_it)) {
			sess_it = _item_get(sid, nsid);
			if (sess_it != NULL &&
					trig_check_keylist(item_data(sess_it), nsid, key, nkey) == TRIG_OK) {
				*item = _item_get(key, nkey);

				if (*item != NULL)
					stats_thread_incr(get_hit);

				status = CO_OK;
				_item_remove(sess_it);
			} else {
				status = CO_RETRY;
				if (sess_it != NULL)
					_item_remove(sess_it);
			}

			_item_remove(iqlease_it);
			if (colease_it != NULL)
				_item_remove(colease_it);
			pthread_mutex_unlock(&cache_lock);
			return status;
		}
	}

	// get session entry from ActiveSessions
	sess_it = _item_get(sid, nsid);

	// associate the key with this session
	_item_assoc_key_sid(c, sess_it, key, nkey, sid, nsid);
	if (sess_it == NULL)
		stats_thread_incr(total_sess);

	// at this point, the key is able to be granted a C lease
	// because it holds neither IQ leases nor O lease
	// get the item
	// grant I lease if necessary
	it = _item_get(key, nkey);
	if (it == NULL || item_data(it) == NULL) {
		iqlease_it = _item_create_lease(key, nkey, new_lease_token);
		if (iqlease_it != NULL) {
			item_set_i_lease(iqlease_it);
			_item_store(iqlease_it, REQ_SET, c, true);
			*item = iqlease_it;
			stats_thread_incr(get_miss);
			stats_thread_incr(total_i_lease);
		}

		if (it != NULL)
			_item_remove(it);
	} else {
		*item = it;
		stats_thread_incr(get_hit);
	}

	// at this point, colease_it may already exist with C lease or not exist
	ASSERT(colease_it == NULL || item_has_c_lease(colease_it));

	// if colease_it does not exist, create a new C colease_it,
	// otherwise add key to the session entry
	_item_assoc_sid_colease(c, colease_it, key, nkey, sid, nsid, C_LEASE);

	if (colease_it != NULL)
		_item_remove(colease_it);

	if (sess_it != NULL)
		_item_remove(sess_it);

	pthread_mutex_unlock(&cache_lock);

	return status;
}

rstatus_t
item_delete_and_release(char* tid, u_int32_t ntid, struct conn *c) {
	struct item* trans_it = NULL;
	struct item* it = NULL;
	trig_cursor_t cursor;
	struct item* lease_it = NULL;

	log_debug(LOG_VERB, "delete_and_release for '%.*s'", ntid, tid);

	pthread_mutex_lock(&cache_lock);

	trans_it = _item_get(tid, ntid);
	if (trans_it == NULL) {
		log_debug(LOG_VERB, "delete_and_release trans item not found '%.*s", tid);

		pthread_mutex_unlock(&cache_lock);
		return MC_INVALID;
	}

	char *data = item_data(trans_it);
	char *key;
	size_t nkey;

	// loop through keys and delete
	cursor = NULL;
	while (trig_keylist_next(data, trans_it->nbyte, &cursor, &key, &nkey) == TRIG_OK) {
		it = _item_get(key, nkey);

		if (it == NULL) {
			// Item may have been evicted due to the cache running out of memory.
			// We should continue to try to delete the rest of the keys.
			log_debug(LOG_VERB, "delete_and_release item not found '%*.s'", nkey, key);
		} else {
			_item_unlink(it);
			_item_remove(it);
		}

		lease_it = _item_get_lease(key, nkey);
		if (lease_it != NULL) {
			if (item_has_q_inv_lease(lease_it)) {
				_item_remove_tid_lease(c, lease_it, key, nkey, tid, ntid);
				_item_remove(lease_it);
			} else {
				_item_unlink(lease_it);
				_item_remove(lease_it);

				stats_thread_incr(released_q_lease);
			}
		}
	}

	_item_unlink(trans_it);
	item_unset_pinned(trans_it);
	_item_remove(trans_it);

	stats_thread_incr(trans_remove);

	pthread_mutex_unlock(&cache_lock);

	return MC_OK;
}

item_iq_result_t
item_commit(char* tid, u_int32_t ntid, struct conn *c, int32_t pending, int32_t server_cfg_id) {
	struct item* trans_it = NULL;
	struct item* it = NULL;
	struct item* pv_it = NULL;
	struct item* lease_it = NULL;
	trig_cursor_t cursor;

	log_debug(LOG_VERB, "commit for transaction '%.*s'", ntid, tid);

	pthread_mutex_lock(&cache_lock);

	// if tid does not exist, return
	trans_it = _item_get(tid, ntid);
	if (trans_it == NULL) {
		log_debug(LOG_VERB, "commit transaction item not found %s", tid);

		pthread_mutex_unlock(&cache_lock);
		return IQ_NOT_FOUND;
	}

	char *data = item_data(trans_it);
	char *key;
	size_t nkey;

	// loop through keys and perform changes
	cursor = NULL;
	while (trig_keylist_next(data, trans_it->nbyte, &cursor, &key, &nkey) == TRIG_OK) {
		lease_it = _item_get_lease(key, nkey);
		pv_it = _item_get_pending_version(key, nkey);

		if (lease_it == NULL) {
			log_debug(LOG_VERB, "commit cannot find lease item of key '%.*s'", nkey, key);
			ASSERT (pv_it == NULL);
			continue;
		}

		if (!item_has_q_lease(lease_it)) {
			log_debug(LOG_VERB, "commit find lease item invalid '%.*s'", nkey, key);
			ASSERT (pv_it == NULL);

			_item_remove(lease_it);
			continue;
		}

		if (item_has_q_inv_lease(lease_it)) {	// this lease is for invalidate transaction
			ASSERT(pv_it == NULL);
			_item_remove_tid_lease(c, lease_it, key, nkey, tid, ntid);
		} else {
			// remove lease item
			_item_unlink(lease_it);
		}

		_item_remove(lease_it);

		// apply changes
		if (pv_it != NULL) {
			int id = item_slabid(nkey, pv_it->nbyte);

			struct item *new_it = _item_alloc_config(id, key, nkey, pv_it->dataflags,
					0, pv_it->nbyte, true, false, server_cfg_id);
			memcpy(item_data(new_it), item_data(pv_it), pv_it->nbyte);

			new_it->p = pending;
			_item_store(new_it, REQ_SET, c, true);
			_item_remove(new_it);
		}

		it = _item_get(key, nkey);

		if (it != NULL) {
			if (pv_it == NULL || item_has_q_inv_lease(lease_it)) {
				_item_unlink(it);
			}
		}

		if (it == NULL)	{	// no value, create a pending item if the pending flag is set.
			struct item* pending_it = _item_get_pending(key, nkey);

			if (pending == 1 && pending_it == NULL) {
				size_t pending_nkey = nkey + PREFIX_KEY_LEN;
				char pending_key[pending_nkey];
				mc_get_pending_key(key, nkey, &pending_key);
				int id = item_slabid(pending_nkey, 1);
				pending_it = _item_alloc_config(id, pending_key, pending_nkey, 0, 0, 1, true, false, server_cfg_id);
				_item_store(pending_it, REQ_SET, c, true);
			} else if (pending == 0 && pending_it != NULL) {
				_item_unlink(pending_it);
			}

			if (pending_it != NULL) {
				_item_remove(pending_it);
			}
		} else {
			struct item* pending_it = _item_get_pending(key, nkey);
			if (pending_it != NULL) {
				_item_unlink(pending_it);
				_item_remove(pending_it);
			}
		}


		if (it != NULL) {
			_item_remove(it);
		}

		// remove pv_it
		if (pv_it != NULL) {
			_item_unlink(pv_it);
			_item_remove(pv_it);
		}
	}

	item_unset_pinned(trans_it);
	_item_unlink(trans_it);
	_item_remove(trans_it);

	stats_thread_incr(trans_remove);

	pthread_mutex_unlock(&cache_lock);

	return IQ_OK;
}

item_iq_result_t
item_release(char* tid, u_int32_t ntid, struct conn *c) {
	struct item* trans_it = NULL;
	struct item* pv_it = NULL;
	struct item* lease_it = NULL;
	trig_cursor_t cursor;

	log_debug(LOG_VERB, "release for transaction '%.*s'", ntid, tid);

	pthread_mutex_lock(&cache_lock);

	// if tid does not exist, return
	trans_it = _item_get(tid, ntid);
	if (trans_it == NULL) {
		log_debug(LOG_VERB, "release transaction item not found %s", tid);

		pthread_mutex_unlock(&cache_lock);
		return IQ_NOT_FOUND;
	}

	char *data = item_data(trans_it);
	char *key;
	size_t nkey;

	// loop through keys and perform changes
	cursor = NULL;
	while (trig_keylist_next(data, trans_it->nbyte, &cursor, &key, &nkey) == TRIG_OK) {
		lease_it = _item_get_lease(key, nkey);
		pv_it = _item_get_pending_version(key, nkey);

		if (lease_it == NULL) {
			log_debug(LOG_VERB, "release cannot find lease item of key '%.*s'", nkey, key);
			ASSERT (pv_it == NULL);
			continue;
		}

		if (!item_has_q_lease(lease_it)) {
			log_debug(LOG_VERB, "release find lease item invalid '%.*s'", nkey, key);
			ASSERT(pv_it == NULL);
			_item_remove(lease_it);
			continue;
		}

		if (item_has_q_inv_lease(lease_it)) {	// this lease is for invalidate transaction
			ASSERT(pv_it == NULL);
			_item_remove_tid_lease(c, trans_it, key, nkey, tid, ntid);
		} else if (item_has_q_incr_lease(lease_it) || item_has_q_ref_lease(lease_it)) {
			stats_thread_incr(released_q_lease);
		}

		// remove lease item
		_item_unlink(lease_it);
		_item_remove(lease_it);

		if (pv_it != NULL) {
			_item_unlink(pv_it);
			_item_remove(pv_it);
		}
	}

	item_unset_pinned(trans_it);
	_item_unlink(trans_it);
	_item_remove(trans_it);

	stats_thread_incr(trans_remove);

	pthread_mutex_unlock(&cache_lock);

	return IQ_OK;
}

/**
 * Try to quarantine and read the item.
 * Return MC_OK if it successfully quarantine and read the item
 */
rstatus_t
item_quarantine_and_read(char* tid, size_t tid_size, char* key, uint8_t nkey,
		lease_token_t lease_token, lease_token_t * new_lease_token,
		struct conn *c, struct item ** it, uint8_t *pending) {
	struct item* lease_it = NULL;
	struct item* trans_it = NULL;
	rstatus_t status = MC_OK;

	log_debug(LOG_VERB, "quarantine_and_read for '%.*s'", nkey, key);

	pthread_mutex_lock(&cache_lock);

	trans_it = _item_get(tid, tid_size); 		// get transaction
	lease_it = _item_get_lease(key, nkey);

	if (lease_it != NULL && (item_has_q_lease(lease_it))) {	// there is a pending Q lease on the key
		if ((item_has_q_ref_lease(lease_it) || item_has_q_incr_lease(lease_it))
				&& _item_lease_value(lease_it) == lease_token) {
			*new_lease_token = lease_token;

			struct item* pv_it = _item_get_pending_version(key, nkey);
			if (pv_it != NULL)
				*it = pv_it;

			if (*it != NULL) {
				*pending = (*it)->p;
			} else {
				struct item* pending_it = _item_get_pending(key, nkey);
				if (pending_it != NULL) {
					*pending = 1;
					_item_remove(pending_it);
				}
			}
			status = MC_OK;
		} else {
			*new_lease_token = LEASE_HOTMISS;
			status = MC_OK;
		}
	} else {
		if (lease_it != NULL) {		// there is a pending I lease on the key
			stats_thread_incr(qlease_voids);
		}

		lease_it = _item_create_lease(key, nkey, new_lease_token);
		item_set_q_ref_lease(lease_it);
		_item_store(lease_it, c->req_type, c, true);

		struct item* orig_it = _item_get(key, nkey);
		if (orig_it != NULL) {
			*pending = (orig_it)->p;

			// create a pending key-value pair
			struct item* pv_it = _item_create_pending_version(orig_it);
			pv_it->p = (orig_it)->p;
			pv_it->exptime = lease_it->exptime;
			memcpy(item_data(pv_it), item_data(orig_it), (orig_it)->nbyte);
			_item_store(pv_it, REQ_SET, c, true);
			_item_remove(orig_it);

			*it = pv_it;
		} else {
			struct item* pending_it = _item_get_pending(key, nkey);
			if (pending_it != NULL) {
				*pending = 1;
				_item_remove(pending_it);
			}
		}

		stats_thread_incr_by(active_q_lease, 1);
		stats_thread_incr(total_lease);
		stats_thread_incr(total_q_lease);

		if (*it != NULL && lease_it != NULL) {
			(*it)->exptime = lease_it->exptime;
		}

		_item_assoc_key_tid(c, trans_it, key, nkey, tid, tid_size);

		status = MC_OK;
	}

	if (status == MC_INVALID) {
		stats_thread_incr(qlease_aborts);
	}

	if (lease_it != NULL)
		_item_remove(lease_it);

	if (trans_it != NULL)
		_item_remove(trans_it);

	pthread_mutex_unlock(&cache_lock);

	return status;
}

void abort_sessions(struct conn* c, struct item* colease_it,
		char* sid, size_t nsid) {
	// mark all pending read sessions as aborted if they are are different with this session
	trig_cursor_t cursor = NULL;
	char* psid = NULL;
	size_t npsid = 0;
	while (trig_keylist_next(item_data(colease_it), colease_it->nbyte,
			&cursor, &psid, &npsid) == TRIG_OK) {
		struct item* psess_it = _item_get(psid, npsid);

		if (psess_it != NULL) {
			if (memcmp(sid, psid, nsid) != 0) {	// different session, mark it as aborted
				psess_it->sess_status = ABORT;
				char* tkey = NULL;
				size_t ntkey = 0;
				trig_cursor_t kcursor = NULL;
				struct item* tcolease_it = NULL;
				struct item* tlease_it = NULL;
				while (trig_keylist_next(item_data(psess_it), psess_it->nbyte, &kcursor, &tkey, &ntkey) == TRIG_OK) {
					tcolease_it = _item_get_co_lease(tkey, ntkey);
					if (tcolease_it != NULL) {
						if (tcolease_it != colease_it &&
								trig_check_keylist(item_data(tcolease_it), tcolease_it->nbyte, psid, npsid) == TRIG_OK) {
							_item_remove_sid_colease(c, tcolease_it, tkey, ntkey, psid, npsid);
						}

						if (tcolease_it != colease_it) {
							tlease_it = _item_get_lease(tkey, ntkey);
							if (tlease_it != NULL) {
								_item_unlink(tlease_it);
								_item_remove(tlease_it);
							}
						}

						_item_remove(tcolease_it);
					}
				}
			} else {		// same session
				// no need to do anything here
				// the colease item will be deleted and then replace by a new one
			}

			_item_remove(psess_it);
		}
	}
}

void clean_session(char* sid, size_t nsid, struct conn* c) {
	char* key;
	size_t nkey = 0;
	struct item* sess_it;
	struct item* lease_it;
	struct item* colease_it;
	struct item* pv_it;
	trig_cursor_t cursor = NULL;

	sess_it = _item_get(sid, nsid);
	if (sess_it == NULL)
		return;

	if (sess_it->sess_status == ABORT) {	// session has been aborted
		_item_unlink(sess_it);
		item_unset_pinned(sess_it);
		_item_remove(sess_it);
		stats_thread_incr(sess_abort);
		pthread_mutex_unlock(&cache_lock);
		return;
	}

	// loop through keys and delete
	while (trig_keylist_next(item_data(sess_it), sess_it->nbyte, &cursor, &key, &nkey) == TRIG_OK) {
		lease_it = _item_get_lease(key, nkey);
		if (lease_it != NULL) {
			_item_unlink(lease_it);
			_item_remove(lease_it);
		}

		// remove colease_it
		colease_it = _item_get_co_lease(key, nkey);
		if (colease_it != NULL) {
			_item_remove_sid_colease(c, colease_it, key, nkey, sid, nsid);
			_item_remove(colease_it);
		}

		pv_it = _item_get_pending_version(key, nkey);
		if (pv_it != NULL) {
			_item_unlink(pv_it);
			_item_remove(pv_it);
		}
	}

	_item_unlink(sess_it);
	item_unset_pinned(sess_it);
	_item_remove(sess_it);

	stats_thread_incr(sess_unlease);
}

/** Grant O and Q leases and get the value **/
item_co_result_t
item_oqread(char* sid, size_t nsid, char* key, uint8_t nkey, struct conn *c, struct item ** it) {
	struct item* sess_it = NULL;
	struct item* lease_it = NULL;
	struct item* colease_it = NULL;
	item_co_result_t status = CO_OK;

	log_debug(LOG_VERB, "oqread for '%.*s'", nkey, key);

	pthread_mutex_lock(&cache_lock);

	sess_it = _item_get(sid, nsid);
	if (sess_it != NULL) {
		if (sess_it->sess_status == ABORT) {
			_item_unlink(sess_it);
			item_unset_pinned(sess_it);
			_item_remove(sess_it);
			stats_thread_incr(sess_abort);
			pthread_mutex_unlock(&cache_lock);
			return CO_ABORT;
		}
		_item_remove(sess_it);
	}

	colease_it = _item_get_co_lease(key, nkey);
	if (colease_it != NULL) {
		ASSERT(item_has_co_lease(colease_it));
		if (item_has_c_lease(colease_it)) {
			abort_sessions(c, colease_it, sid, nsid);
			_item_unlink(colease_it);
			_item_remove(colease_it);
			colease_it = NULL;
		} else if (item_has_o_lease(colease_it)) {
			if (trig_check_keylist(item_data(colease_it), colease_it->nbyte, sid, nsid) == TRIG_OK) {	// same session
				*it = _item_get(key, nkey);
				_item_remove(colease_it);
				pthread_mutex_unlock(&cache_lock);
				return CO_OK;
			} else {
				// clean up session
				_item_remove(colease_it);
				clean_session(sid, nsid, c);
				pthread_mutex_unlock(&cache_lock);
				return CO_ABORT;
			}
		}
	}

	lease_it = _item_get_lease(key, nkey);
	if (lease_it != NULL && (item_has_q_ref_lease(lease_it))) {	// there is a pending Q lease on the key
		_item_remove(lease_it);
		status = CO_ABORT;
	} else {
		*it = _item_get(key, nkey);

		if (lease_it != NULL) {		// there is a pending I lease on the key
			stats_thread_incr(qlease_voids);
		}

		// allocate memory for new item
		size_t lease_nkey = nkey + PREFIX_KEY_LEN;
		char lease_key[lease_nkey];
		mc_get_lease_key(key, nkey, &lease_key);
		lease_it = _item_create_reserved_item(lease_key, lease_nkey, 0, true);
		ASSERT(lease_it != NULL);
		item_set_q_ref_lease(lease_it);
		_item_store(lease_it, REQ_SET, c, true);

		stats_thread_incr_by(active_q_lease, 1);
		stats_thread_incr(total_lease);
		stats_thread_incr(total_q_lease);

		if (*it != NULL && lease_it != NULL) {
			(*it)->exptime = lease_it->exptime;
		}

		status = CO_OK;
	}

	if (lease_it != NULL)
		_item_remove(lease_it);

	if (status == CO_OK) {
		sess_it = _item_get(sid, nsid);
		_item_assoc_key_sid(c, sess_it, key, nkey, sid, nsid);
		colease_it = _item_get_co_lease(key, nkey);
		_item_assoc_sid_colease(c, colease_it, key, nkey, sid, nsid, O_LEASE_REF);
	}

	pthread_mutex_unlock(&cache_lock);

	return status;
}

item_co_result_t
item_oqswap(char* sid, size_t nsid, struct item* it, struct conn* c) {
	struct item* sess_it = NULL;
	struct item* colease_it = NULL;
	item_co_result_t status = CO_OK;
	struct item* lease_it = NULL;
	struct item* pv_it = NULL;

	log_debug(LOG_VERB, "oq_swap_and_release for '%.*s'", it->nkey, item_key(it));

	pthread_mutex_lock(&cache_lock);

	sess_it = _item_get(sid, nsid);
	if (sess_it != NULL && sess_it->sess_status == ABORT) {
		_item_unlink(sess_it);
		item_unset_pinned(sess_it);
		_item_remove(sess_it);
		pthread_mutex_unlock(&cache_lock);
		return CO_ABORT;
	}

	if (sess_it != NULL)
		_item_remove(sess_it);

	colease_it = _item_get_co_lease(item_key(it), it->nkey);
	if (colease_it == NULL || item_has_c_lease(colease_it)) {
		if (colease_it != NULL)
			_item_remove(colease_it);
		pthread_mutex_unlock(&cache_lock);
		return CO_ABORT;
	} else if (item_has_o_lease(colease_it)) {
		if (trig_check_keylist(item_data(colease_it), colease_it->nbyte, sid, nsid) != TRIG_OK) {
			_item_unlink(colease_it);
			_item_remove(colease_it);
			pthread_mutex_unlock(&cache_lock);
			return CO_ABORT;
		}
		_item_remove(colease_it);
	} else {
		_item_unlink(colease_it);
		_item_remove(colease_it);
		pthread_mutex_unlock(&cache_lock);
		return CO_INVALID;
	}

	// delete the lease first
	lease_it = _item_get_lease(item_key(it), it->nkey);

	// lease item does not exist or the token does not match
	if (lease_it == NULL || !item_has_q_ref_lease(lease_it)) {
		status = CO_INVALID;

		if (lease_it != NULL)
			_item_remove(lease_it);

		pthread_mutex_unlock(&cache_lock);
		return status;
	} else {
		_item_unlink(lease_it);
		_item_remove(lease_it);
		stats_thread_incr(released_q_lease);
	}

	if (it == NULL || it->nbyte == 0) {
		pthread_mutex_unlock(&cache_lock);
		return CO_INVALID;
	}

	if (it != NULL) {
		pv_it = _item_create_pending_version(it);
		memcpy(item_data(pv_it), item_data(it), it->nbyte);
		_item_store(pv_it, REQ_SET, c, true);
		_item_remove(pv_it);
	}

	// no need to remove ref count for it because it is handled at
	// the function asc_complete_nread

	pthread_mutex_unlock(&cache_lock);
	return status;
}

item_co_result_t
item_oqwrite(char* sid, size_t nsid, struct item* it, struct conn* c) {
	struct item* sess_it = NULL;
	struct item* colease_it = NULL;
	item_co_result_t status = CO_OK;
	struct item* lease_it = NULL;
	struct item* pv_it = NULL;

	log_debug(LOG_VERB, "oq_write for '%.*s'", it->nkey, item_key(it));

	pthread_mutex_lock(&cache_lock);

	sess_it = _item_get(sid, nsid);
	if (sess_it != NULL && sess_it->sess_status == ABORT) {
		_item_unlink(sess_it);
		item_unset_pinned(sess_it);
		_item_remove(sess_it);
		pthread_mutex_unlock(&cache_lock);
		return CO_ABORT;
	}

	if (sess_it != NULL)
		_item_remove(sess_it);

	colease_it = _item_get_co_lease(item_key(it), it->nkey);

	if (colease_it != NULL && item_has_c_lease(colease_it)) {
		abort_sessions(c, colease_it, sid, nsid);
		_item_unlink(colease_it);
		_item_remove(colease_it);

		lease_it = _item_get_lease(item_key(it), it->nkey);
		if (lease_it != NULL) {
			_item_unlink(lease_it);
			_item_remove(lease_it);
			lease_it = NULL;
		}

		colease_it = NULL;
	}

	if (colease_it != NULL) {
		ASSERT(item_has_o_lease(colease_it));
		if (trig_check_keylist(item_data(colease_it), colease_it->nbyte, sid, nsid) != TRIG_OK) {
			_item_remove(colease_it);
			clean_session(sid, nsid, c);
			pthread_mutex_unlock(&cache_lock);
			return CO_ABORT;
		}
		_item_remove(colease_it);
	}

	if (colease_it == NULL) {
		_item_assoc_sid_colease(c, colease_it, item_key(it), it->nkey, sid, nsid, O_LEASE_REF);
	} else {
		// delete the lease first
		lease_it = _item_get_lease(item_key(it), it->nkey);

		// lease item has another q lease
		// this should never happen because the CO lease hints that
		// there is no Q lease at this point.
		if (lease_it != NULL && !item_has_q_ref_lease(lease_it)) {
			status = CO_INVALID;

			if (lease_it != NULL)
				_item_remove(lease_it);

			pthread_mutex_unlock(&cache_lock);
			return status;
		} else {
			if (lease_it != NULL) {
				_item_unlink(lease_it);
				_item_remove(lease_it);
			}

			stats_thread_incr(released_q_lease);
		}
	}

	if (it == NULL || it->nbyte == 0) {	// try to set null value
		if (it != NULL) {
			pv_it = _item_get_pending_version(item_key(it), it->nkey);
			if (pv_it != NULL) {
				_item_unlink(pv_it);
				_item_remove(pv_it);
			}
		}
	} else {
		pv_it = _item_create_pending_version(it);
		memcpy(item_data(pv_it), item_data(it), it->nbyte);
		_item_store(pv_it, REQ_SET, c, true);
		_item_remove(pv_it);
	}

	sess_it = _item_get(sid, nsid);
	_item_assoc_key_sid(c, sess_it, item_key(it), it->nkey, sid, nsid);
	if (sess_it != NULL)
		_item_remove(sess_it);

	// no need to remove ref count for it because it is handled at
	// the function asc_complete_nread

	pthread_mutex_unlock(&cache_lock);
	return status;
}

item_store_result_t
item_swap_and_release(struct item* it, struct conn *c) {
	struct item *oit = NULL;
	item_store_result_t status = STORED;
	struct item* lease_it = NULL;

	log_debug(LOG_VERB, "swap_and_release for '%.*s'", it->nkey, item_key(it));

	pthread_mutex_lock(&cache_lock);

	// delete the lease first
	lease_it = _item_get_lease(item_key(it), it->nkey);

	// lease item does not exist or the token does not match
	if (lease_it == NULL || _item_lease_value(lease_it) != c->lease_token) {
		status = NOT_FOUND;

		if (lease_it != NULL)
			_item_remove(lease_it);

		pthread_mutex_unlock(&cache_lock);
		return status;
	} else {
		_item_unlink(lease_it);
		_item_remove(lease_it);

		stats_thread_incr(released_q_lease);
	}

	if (it == NULL || it->nbyte == 0) {
		pthread_mutex_unlock(&cache_lock);
		return STORE_ERROR;
	}

	oit = _item_get(item_key(it), it->nkey);

	if (oit == NULL) {
		_item_store(it, REQ_SET, c, true);

		status = STORED;
	} else {
		// create new item at normal space and replace old item
		_item_replace(oit, it);

		status = STORED;
	}

	if (oit != NULL) {
		_item_remove(oit);
	}

	// no need to remove ref count for it because it is handled at
	// the function asc_complete_nread

	pthread_mutex_unlock(&cache_lock);

	return status;
}


item_store_result_t
item_swap(struct item* it, struct conn *c) {
	struct item *pv_it = NULL;
	item_store_result_t status = STORED;
	struct item* lease_it = NULL;

	log_debug(LOG_VERB, "swap for '%.*s'", it->nkey, item_key(it));

	pthread_mutex_lock(&cache_lock);

	// delete the lease first
	lease_it = _item_get_lease(item_key(it), it->nkey);

	// lease item does not exist or the token does not match
	if (lease_it == NULL || _item_lease_value(lease_it) != c->lease_token) {
		status = NOT_FOUND;

		if (lease_it != NULL)
			_item_remove(lease_it);

		pthread_mutex_unlock(&cache_lock);
		return status;
	} else {
		_item_remove(lease_it);
	}

	if (it == NULL || it->nbyte == 0) {
		pthread_mutex_unlock(&cache_lock);
		return STORED;
	}

	uint8_t pending = 0;
	pv_it = _item_get_pending_version(item_key(it), it->nkey);
	if (pv_it != NULL) {
		pending = pv_it->p;

		_item_unlink(pv_it);
		_item_remove(pv_it);
	} else {
		struct item* pending_it = _item_get_pending(item_key(it), it->nkey);
		if (pending_it != NULL) {
			pending = 1;
			_item_unlink(pending_it);
			_item_remove(pending_it);
		}
	}

	pv_it = _item_create_pending_version(it);
	memcpy(item_data(pv_it), item_data(it), it->nbyte);
	pv_it->p = pending;
	_item_store(pv_it, REQ_SET, c, true);
	_item_remove(pv_it);

	status = STORED;

	// no need to remove ref count for it because it is handled at
	// the function asc_complete_nread

	pthread_mutex_unlock(&cache_lock);

	return status;
}

void item_ftrans(char*tid, size_t tid_size, char*key, size_t key_size, struct conn* c) {
	pthread_mutex_lock(&cache_lock);
	struct item* ptrans_it = _item_get_ptrans(key, key_size);
	_item_remove_tid_ptrans(c, ptrans_it, key, key_size, tid, tid_size);
	if (ptrans_it != NULL) {
		_item_remove(ptrans_it);
	}
	pthread_mutex_unlock(&cache_lock);
}

item_iq_result_t
item_iqget(char *key, size_t nkey, lease_token_t lease_token,
		char* tid, size_t tid_size, struct conn *c,
		struct item** item, lease_token_t* new_lease_token, int override, int foreground) {
	struct item* it = NULL;
	struct item* pv_it = NULL;	// new version of the item
	item_iq_result_t status = IQ_NO_VALUE;
	struct item* trans_it = NULL;
	struct item* lease_it = NULL;

	pthread_mutex_lock(&cache_lock);

	log_debug(LOG_VERB, "iqget for '%.*s'", nkey, key);

	if (foreground == 1) {
		stats_thread_incr(iqget);
	}
	*item = NULL;

	// get trans and it item
	if (tid_size > 0)
		trans_it = _item_get(tid, tid_size);
	it = _item_get(key, nkey);

	// get pending information
	uint8_t pending = 0;
	if (it == NULL) {
		struct item* pending_it = _item_get_pending(key, nkey);
		if (pending_it != NULL) {
			pending = 1;
			_item_remove(pending_it);
		} else {
			pending = 0;
		}
	} else {
		pending = it->p;
	}

	// transaction does not exist or the key does not belong to this transaction
	if (trans_it == NULL || trig_check_keylist(item_data(trans_it), trans_it->nbyte, key,
			(trig_keylen_t) nkey) != TRIG_OK) {
		if (it != NULL) {
			ASSERT (it->nbyte != 0);

			// check if there is a q lease on the item
			lease_it = _item_get_lease(key, nkey);
			if (lease_it != NULL && item_has_q_ref_lease(lease_it) &&
					_item_lease_value(lease_it) == lease_token) {
				status = IQ_NO_VALUE;
				stats_thread_incr(iqget_miss);
			} else {
				if (ILEASE_MEET_QLEASE && lease_it != NULL && item_has_q_ref_lease(lease_it) && _item_lease_value(lease_it) != lease_token) {
					stats_thread_incr(iqget_meet_qlease);
					*item = lease_it;
					*new_lease_token = LEASE_HOTMISS;
					status = IQ_MISS;
				} else {
					if (override == 1) {
						if (lease_it == NULL) {	// no lease
							lease_it = _item_create_lease(key, nkey,
									new_lease_token);

							if (lease_it != NULL) {
								item_set_i_lease(lease_it);
								_item_unlink(it);
								_item_store(lease_it, REQ_SET, c, true);

								stats_thread_incr(total_lease);
								stats_thread_incr(total_i_lease);

								lease_it->p = pending;
								*item = lease_it;
								status = IQ_LEASE;
							} else
								status = IQ_SERVER_ERROR;
						} else {// item has lease (maybe I lease, maybe Q lease for refresh technique)
							lease_it->p = pending;
							*item = lease_it;

							lease_token_t token = _item_lease_value(lease_it);

							if (token == lease_token)
								status = IQ_NO_VALUE;
							else {
								stats_thread_incr(ilease_aborts);
								*new_lease_token = LEASE_HOTMISS;
								status = IQ_MISS;
							}
						}
					} else {
						_item_touch(it);
						*item = it;

						if (foreground == 1) {
							stats_slab_incr(it->id, get_hit);
							stats_thread_incr(iqget_hit);
						}

						status = IQ_VALUE;
					}
				}
			}
		} else {
			stats_thread_incr(iqget_miss);
			lease_it = _item_get_lease(key, nkey);

			if (lease_it == NULL) {	// no lease
				lease_it = _item_create_lease(key, nkey, new_lease_token);

				if (lease_it != NULL) {
					item_set_i_lease(lease_it);
					_item_store(lease_it, REQ_SET, c, true);

					if (it != NULL)
						it->exptime = lease_it->exptime;

					stats_thread_incr(total_lease);
					stats_thread_incr(total_i_lease);

					lease_it->p = pending;
					*item = lease_it;
					status = IQ_LEASE;
				} else
					status = IQ_SERVER_ERROR;
			} else {		// item has lease (maybe I lease, maybe Q lease for refresh technique)
				lease_it->p = pending;
				*item = lease_it;

				lease_token_t token = _item_lease_value(lease_it);

				if (token == lease_token)
					status = IQ_NO_VALUE;
				else {
					stats_thread_incr(ilease_aborts);
					*new_lease_token = LEASE_HOTMISS;

					status = IQ_MISS;
				}
			}
		}
	} else {
		lease_it = _item_get_lease(key, nkey);

		if (lease_it == NULL) {
			// lease item not found for some reasons (for example, delete is called)
			// return NO_VALUE suggesting the client to query the RDBMS for the value
			// TODO: should be revised
			status = IQ_NO_VALUE;
		} else {
			ASSERT (item_has_q_inv_lease(lease_it) || item_has_q_incr_lease(lease_it));

			if (item_has_q_inv_lease(lease_it)) {
				status = IQ_NO_VALUE;		// iqget from the same transaction
				// do not try to read the value because we want read ops
				// of the same session to update its own update.
			} else if (item_has_q_incr_lease(lease_it)) {
				pv_it = _item_get_pending_version(key, nkey);

				if (pv_it != NULL) {
					ASSERT (pv_it->nbyte != 0);

					if (foreground == 1) {
						stats_slab_incr(it->id, get_hit);
						stats_thread_incr(iqget_hit);
					}
					*item = pv_it;
					status = IQ_VALUE;
				} else {
					stats_thread_incr(iqget_miss);
					status = IQ_NO_VALUE;
				}
			}
		}
	}

	if (*item != NULL) {
		item_acquire_refcount(*item);
	}

	if (trans_it != NULL) {
		_item_remove(trans_it);
	}

//	if (pv_it != NULL) {
//		_item_remove(pv_it);
//	}

	if (it != NULL) {
		_item_remove(it);
	}

	if (lease_it != NULL) {
		_item_remove(lease_it);
	}

	pthread_mutex_unlock(&cache_lock);

	return status;
}

item_iq_result_t
item_iqincr_iqdecr(struct conn *c, char *key, size_t nkey, bool incr,
		int64_t delta, char *tid, size_t ntid, uint8_t *pending, uint64_t *new_lease_token) {
	item_iq_result_t ret;

	pthread_mutex_lock(&cache_lock);
	ret = _item_iqincr_iqdecr(c, key, nkey, incr, delta, tid, ntid,
			pending, new_lease_token);
	pthread_mutex_unlock(&cache_lock);

	return ret;
}

item_co_result_t
item_oqincr_oqdecr(struct conn *c, char *key, size_t nkey, bool incr,
		int64_t delta, char *sid, size_t nsid, uint64_t *val) {
	struct item* pv_it;
	item_co_result_t ret;
	char* ptr;
	pthread_mutex_lock(&cache_lock);
	ret = _item_oqincr_oqdecr(c, key, nkey, incr, delta, sid, nsid);
	pv_it = _item_get_pending_version(key, nkey);
	if (ret == CO_OK && pv_it != NULL) {
		ptr = item_data(pv_it);
		mc_strtoull_len(ptr, val, pv_it->nbyte);
//		_item_remove(pv_it);
	} else {
		if (ret == CO_OK)
			ret = CO_NOVALUE;
	}
	if (pv_it != NULL) {
		_item_remove(pv_it);
	}
	pthread_mutex_unlock(&cache_lock);
	return ret;
}

item_iq_result_t
item_iqappend_iqprepend(struct conn *c,
		struct item* item, uint8_t *pending, uint64_t *new_lease_token) {
	struct item* it = NULL;
	struct item* trans_it = NULL;
	struct item* pv_it = NULL;
	struct item* lease_it = NULL;
	item_iq_result_t ret = IQ_LEASE;
	char *tid = c->tid;
	size_t ntid = c->ntid;
	char *key = item_key(item);
	size_t nkey = item->nkey;

	log_debug(LOG_VERB, "iq_iqappend_iqprepend for tid '%.*s' key '%.*s'", ntid, tid, nkey, key);

	pthread_mutex_lock(&cache_lock);

	switch (c->req_type) {
	case REQ_IQAPPEND:
		stats_thread_incr(iqappend);
		break;
	case REQ_IQPREPEND:
		stats_thread_incr(iqprepend);
		break;
	default:
		break;
	}

	// get trans and it item
	it = _item_get(key, nkey);
	trans_it = _item_get(tid, ntid);
	lease_it = _item_get_lease(key, nkey);

	if (lease_it == NULL || item_has_i_lease(lease_it)) {
		if (ret != IQ_ABORT) {
			if (lease_it != NULL) {
				_item_unlink(lease_it);
				_item_remove(lease_it);

				stats_thread_incr(qlease_voids);
			}

			// create a new lease item
			lease_it = _item_create_lease(key, nkey, new_lease_token);
			item_set_q_inc_lease(lease_it);

			item_store_result_t stored = _item_store(lease_it, REQ_SET, c, true);
			if (stored == NOT_STORED) {
				log_debug(LOG_VERB, "cannot store item '%.*s'", lease_it->nkey, item_key(lease_it));
				ret = IQ_SERVER_ERROR;
			} else {
				stats_thread_incr(active_q_lease);
				stats_thread_incr(total_lease);
				stats_thread_incr(total_q_lease);

				// associate the key with the tid
				_item_assoc_key_tid(c, trans_it, key, nkey, tid, ntid);

				if (it != NULL) {
					ASSERT (it->nbyte != 0);

//					it->exptime = lease_it->exptime;

					// create a pending version of the key
					pv_it = _item_create_pending_version(it);
					pv_it->p = it->p;
					pv_it->exptime = lease_it->exptime;
					*pending = it->p;
					memcpy(item_data(pv_it), item_data(it), it->nbyte);
					_item_store(pv_it, REQ_SET, c, true);

					ret = _item_append_prepend_iq(c, pv_it, item_data(item), item->nbyte);
				} else {
					// check pending
					struct item* pending_it = _item_get_pending(key, nkey);
					if (pending_it == NULL)
						*pending = 0;
					else {
						*pending = 1;
						_item_remove(pending_it);
					}

					ret = IQ_LEASE_NO_VALUE;
				}
			}
		}
	} else {
		ASSERT (item_has_q_lease(lease_it));

		trig_keylen_t key_len = nkey;

		if (trans_it == NULL || trig_check_keylist(item_data(trans_it), trans_it->nbyte, key, key_len) == TRIG_NOTFOUND) {

			stats_thread_incr(qlease_aborts);

			//_item_release(tid, ntid);

			ret = IQ_ABORT;
		} else {
			*new_lease_token = _item_lease_value(lease_it);

			// get the pending version of the key
			pv_it = _item_get_pending_version(key, nkey);

			if (pv_it == NULL) {	// no version of key but it is currently in the same session, so return NO_LEASE
				struct item* pending_it = _item_get_pending(key, nkey);

				if (pending_it == NULL)
					*pending = 0;
				else {
					*pending = 1;
					_item_remove(pending_it);
				}

				ret = IQ_LEASE_NO_VALUE;
			} else {
				*pending = pv_it->p;
				ret = _item_append_prepend_iq(c, pv_it, item_data(item), item->nbyte);
			}
		}
	}

	if (it != NULL)
		_item_remove(it);

	if (trans_it != NULL)
		_item_remove(trans_it);

	if (pv_it != NULL)
		_item_remove(pv_it);

	if (lease_it != NULL)
		_item_remove(lease_it);

	pthread_mutex_unlock(&cache_lock);

	return ret;
}

item_co_result_t
item_oqappend_oqprepend(struct conn *c, struct item* item) {
	struct item* it = NULL;
	struct item* sess_it = NULL;
	struct item* pv_it = NULL;
	struct item* lease_it = NULL;
	struct item* colease_it = NULL;
	item_co_result_t ret = CO_OK;
	char *sid = c->tid;
	size_t nsid = c->ntid;
	char *key = item_key(item);
	size_t nkey = item->nkey;

	log_debug(LOG_VERB, "item_coappend_coprepend for sid '%.*s' key '%.*s'", nsid, sid, nkey, key);

	pthread_mutex_lock(&cache_lock);

	switch (c->req_type) {
	case REQ_OQAPPEND:
		stats_thread_incr(oqappend);
		break;
	case REQ_OQPREPEND:
		stats_thread_incr(oqprepend);
		break;
	default:
		break;
	}

	sess_it = _item_get(sid, nsid);
	if (sess_it != NULL) {
		if (sess_it->sess_status == ABORT) {
			_item_unlink(sess_it);
			item_unset_pinned(sess_it);
			_item_remove(sess_it);
			stats_thread_incr(sess_abort);
			pthread_mutex_unlock(&cache_lock);
			return CO_ABORT;
		}
		_item_remove(sess_it);
		sess_it = NULL;
	}

	colease_it = _item_get_co_lease(key, nkey);
	if (colease_it != NULL) {
		ASSERT(item_has_co_lease(colease_it));
		if (item_has_c_lease(colease_it)) {
			abort_sessions(c, colease_it, sid, nsid);
			_item_unlink(colease_it);
			_item_remove(colease_it);
			colease_it = NULL;
		} else if (item_has_o_lease(colease_it)) {
			if (trig_check_keylist(item_data(colease_it), colease_it->nbyte, sid, nsid) != TRIG_OK) {	// same session
				// clean up session
				_item_remove(colease_it);
				clean_session(sid, nsid, c);
				pthread_mutex_unlock(&cache_lock);
				return CO_ABORT;
			}
		}
	}

	it = _item_get(key, nkey);
	lease_it = _item_get_lease(key, nkey);

	if (lease_it == NULL || item_has_i_lease(lease_it)) {
		if (lease_it != NULL) {
			_item_unlink(lease_it);
			stats_thread_incr(qlease_voids);
		}

		// create a pending version of the key
		pv_it = _item_get_pending_version(key, nkey);
		if (pv_it == NULL && it != NULL) {
			ASSERT (it->nbyte != 0);
			pv_it = _item_create_pending_version(it);
			memcpy(item_data(pv_it), item_data(it), it->nbyte);
			_item_store(pv_it, REQ_SET, c, true);
		}

		if (pv_it != NULL)
			_item_append_prepend_co(c, pv_it, item_data(item), item->nbyte);
	}

	// associate the key with the sid
	if (colease_it == NULL) {
		_item_assoc_sid_colease(c, colease_it, key, nkey, sid, nsid, O_LEASE_REF);
	}

	sess_it = _item_get(sid, nsid);
	_item_assoc_key_sid(c, sess_it, key, nkey, sid, nsid);
	if (sess_it != NULL)
		_item_remove(sess_it);

	if (it != NULL && item_is_linked(it))
		_item_remove(it);

	if (pv_it != NULL && item_is_linked(pv_it))
		_item_remove(pv_it);

	if (lease_it != NULL)
		_item_remove(lease_it);

	if (colease_it != NULL)
		_item_remove(colease_it);

	pthread_mutex_unlock(&cache_lock);

	return ret;
}

item_co_result_t
item_oqreg(struct conn *c, char *sid, size_t nsid, char *key, size_t nkey) {
	struct item* lease_it = NULL;
	struct item* it = NULL;
	struct item* sess_it = NULL;
	struct item* colease_it = NULL;
	item_co_result_t status = CO_OK;

	log_debug(LOG_VERB, "oqreg for '%.*s'", nkey, key);

	pthread_mutex_lock(&cache_lock);

	sess_it = _item_get(sid, nsid);
	if (sess_it != NULL && sess_it->sess_status == ABORT) {
		_item_unlink(sess_it);
		item_unset_pinned(sess_it);
		_item_remove(sess_it);
		stats_thread_incr(sess_abort);
		pthread_mutex_unlock(&cache_lock);
		return CO_ABORT;
	}

	if (sess_it != NULL)
		_item_remove(sess_it);

	lease_it = _item_get_lease(key, nkey);	// get lease item

	// at this time, transaction item for this session should be ready
	// now it's time to reason about the lease item
	// no matter lease token exists or not, create a new one and overwrite the old one
	if (lease_it != NULL && item_has_i_lease(lease_it)) {
		_item_unlink(lease_it);
		_item_remove(lease_it);
		lease_it = NULL;
		stats_thread_incr(qlease_voids);
	}

	// update the lease time for item
	it = _item_get(key, nkey);
	if (it != NULL) {
		//		it->exptime = lease_it->exptime;
		_item_remove(it);
	}

	if (lease_it == NULL) {
		// allocate memory for new item
		size_t lease_nkey = nkey + PREFIX_KEY_LEN;
		char lease_key[lease_nkey];
		mc_get_lease_key(key, nkey, &lease_key);
		lease_it = _item_create_reserved_item(lease_key, lease_nkey, 0, true);
		ASSERT(lease_it != NULL);
		item_set_q_inv_lease(lease_it);
		_item_store(lease_it, REQ_SET, c, true);
		_item_remove(lease_it);
	}

	// ========================= handle CO lease ============== //
	colease_it = _item_get_co_lease(key, nkey);
	if (colease_it == NULL) {
		// CO lease does not exist, create a new one
		_item_assoc_sid_colease(c, colease_it, key, nkey, sid, nsid, O_LEASE_INV);
		colease_it = _item_get_co_lease(key, nkey);
	}

	ASSERT(colease_it != NULL && item_has_co_lease(colease_it));

	// item already has C lease
	if (item_has_c_lease(colease_it)) {
		abort_sessions(c, colease_it, sid, nsid);
		_item_unlink(colease_it);
		_item_remove(colease_it);
		colease_it = NULL;
		_item_assoc_sid_colease(c, colease_it, key, nkey, sid, nsid, O_LEASE_INV);
	}

	// get session from ActiveSessions
	sess_it = _item_get(sid, nsid);
	_item_assoc_key_sid(c, sess_it, key, nkey, sid, nsid);
	if (sess_it == NULL) {
		stats_thread_incr(total_sess);
	} else {
		_item_remove(sess_it);
	}

	if (colease_it != NULL)
		_item_remove(colease_it);

	pthread_mutex_unlock(&cache_lock);

	return status;
}

item_co_result_t
item_dcommit(char *sid, size_t nsid, struct conn *c) {
	struct item* it = NULL;
	struct item* pv_it = NULL;
	trig_cursor_t cursor = NULL;
	struct item* lease_it = NULL;
	struct item *sess_it = NULL;
	struct item *colease_it = NULL;
	char *key = NULL;
	size_t nkey = 0;

	log_debug(LOG_VERB, "dcommit for '%.*s'", nsid, sid);

	pthread_mutex_lock(&cache_lock);

	sess_it = _item_get(sid, nsid);

	if (sess_it == NULL) {
		log_debug(LOG_VERB, "dcommit session not found '%.*s'", nsid, sid);
		pthread_mutex_unlock(&cache_lock);
		return CO_NOT_FOUND;
	}

	// session has been aborted
	if (sess_it->sess_status == ABORT) {
		_item_unlink(sess_it);
		item_unset_pinned(sess_it);
		_item_remove(sess_it);
		stats_thread_incr(sess_abort);
		pthread_mutex_unlock(&cache_lock);
		return CO_ABORT;
	}

	if (sess_it != NULL) {
		while (trig_keylist_next(item_data(sess_it), sess_it->nbyte, &cursor, &key, &nkey) == TRIG_OK) {
			// delete key if exists
			it = _item_get(key, nkey);

			// delete lease if exists
			lease_it = _item_get_lease(key, nkey);

			if (lease_it != NULL) {
				if (item_has_q_lease(lease_it)) {
					stats_thread_incr(released_q_lease);
				}

				_item_unlink(lease_it);
				_item_remove(lease_it);
			}

			colease_it = _item_get_co_lease(key, nkey);
			if (colease_it != NULL) {
				if (item_has_o_lease_inv(colease_it)) {
					if (it != NULL) {
						_item_unlink(it);
					}
				} else if (item_has_o_lease_ref(colease_it)) {
					pv_it = _item_get_pending_version(key, nkey);
					if (pv_it != NULL) {
						int id = item_slabid(nkey, pv_it->nbyte);
						struct item *new_it = _item_alloc(id, key, nkey, pv_it->dataflags,
								0, pv_it->nbyte, true, false);
						memcpy(item_data(new_it), item_data(pv_it), pv_it->nbyte);
						_item_store(new_it, REQ_SET, c, true);
						_item_remove(new_it);

						_item_unlink(pv_it);
						_item_remove(pv_it);
					} else {
						if (it != NULL)
							_item_unlink(it);
					}
				} else if (item_has_c_lease(colease_it)) {
					if (it != NULL)
						it->exptime = 0;
				}

				// remote sid from CO lease item
				_item_remove_sid_colease(c, colease_it, key, nkey, sid, nsid);
				_item_remove(colease_it);
			}

			if (it != NULL)
				_item_remove(it);
		}
	}

	stats_thread_incr(sess_commit);
	if (sess_it != NULL) {
		_item_unlink(sess_it);
		_item_remove(sess_it);
	}

	pthread_mutex_unlock(&cache_lock);

	return CO_OK;
}

item_co_result_t
item_validate(char *sid, size_t nsid, struct conn *c) {
	trig_cursor_t cursor = NULL;
	struct item* sess_it = NULL;
	struct item* colease_it = NULL;
	char *key = NULL;
	size_t nkey = 0;
	item_co_result_t status = CO_OK;

	log_debug(LOG_VERB, "validate for '%.*s'", nsid, sid);

	pthread_mutex_lock(&cache_lock);

	sess_it = _item_get(sid, nsid);
	if (sess_it == NULL) {
		log_debug(LOG_VERB, "validate session not found '%.*s'", nsid, sid);
		pthread_mutex_unlock(&cache_lock);
		return CO_ABORT;
	}

	// session has been aborted
	if (sess_it->sess_status == ABORT) {
		_item_unlink(sess_it);
		item_unset_pinned(sess_it);
		_item_remove(sess_it);
		stats_thread_incr(sess_abort);
		pthread_mutex_unlock(&cache_lock);
		return CO_ABORT;
	}

	if (sess_it != NULL)
		_item_remove(sess_it);

	while (trig_keylist_next(item_data(sess_it),
			sess_it->nbyte, &cursor, &key, &nkey) == TRIG_OK) {
		colease_it = _item_get_co_lease(key, nkey);
		if (colease_it != NULL) {
			if (trig_check_keylist(item_data(colease_it), colease_it->nbyte,
					sid, nsid) != TRIG_OK) {
				status = CO_ABORT;
			}
			_item_remove(colease_it);
		}
	}

	if (status == CO_ABORT)
		clean_session(sid, nsid, c);

	pthread_mutex_unlock(&cache_lock);
	return status;
}

item_iq_result_t
item_co_unlease(char *sid, size_t nsid, struct conn *c) {
	trig_cursor_t cursor = NULL;
	struct item* lease_it = NULL;
	struct item* sess_it = NULL;
	struct item* colease_it = NULL;
	struct item* pv_it = NULL;
	char* key;
	size_t nkey;

	log_debug(LOG_VERB, "co_unlease for sid '%.*s'", nsid, sid);

	pthread_mutex_lock(&cache_lock);

	sess_it = _item_get(sid, nsid);
	if (sess_it == NULL) {
		log_debug(LOG_VERB, "co_unlease sess item not found '%.*s", nsid, sid);
		pthread_mutex_unlock(&cache_lock);
		return CO_NOT_FOUND;
	} else {
		if (sess_it->sess_status == ABORT) {	// session has been aborted
			_item_unlink(sess_it);
			item_unset_pinned(sess_it);
			_item_remove(sess_it);
			stats_thread_incr(sess_abort);
			pthread_mutex_unlock(&cache_lock);
			return CO_ABORT;
		}

		// loop through keys and delete
		while (trig_keylist_next(item_data(sess_it), sess_it->nbyte, &cursor, &key, &nkey) == TRIG_OK) {
			lease_it = _item_get_lease(key, nkey);
			if (lease_it != NULL) {
				_item_unlink(lease_it);
				_item_remove(lease_it);
			}

			// remove colease_it
			colease_it = _item_get_co_lease(key, nkey);
			if (colease_it != NULL) {
				_item_remove_sid_colease(c, colease_it, key, nkey, sid, nsid);
				_item_remove(colease_it);
			}

			pv_it = _item_get_pending_version(key, nkey);
			if (pv_it != NULL) {
				_item_unlink(pv_it);
				_item_remove(pv_it);
			}
		}
	}

	_item_unlink(sess_it);
	item_unset_pinned(sess_it);
	_item_remove(sess_it);

	stats_thread_incr(sess_unlease);

	pthread_mutex_unlock(&cache_lock);
	return CO_OK;
}

item_iq_result_t
_item_append_prepend_iq(struct conn *c, struct item* it, char* data, size_t nbyte) {
	struct item* nit = NULL;
	size_t total_size;

	ASSERT (it != NULL && it->nbyte != 0);

	total_size = it->nbyte + nbyte;
	uint8_t id = item_slabid(it->nkey, total_size);

	nit = _item_alloc(id, item_key(it), it->nkey, it->dataflags, it->exptime, total_size, true, false);
	nit->p = it->p;

	if (nit == NULL) {
		log_debug(LOG_VERB, "_iq_append_prepend_iq cannot allocate memory for item '%.*s'", it->nkey, item_key(it));
		return IQ_SERVER_ERROR;
	}

	switch (c->req_type) {
	case REQ_IQAPPEND:
		memcpy(item_data(nit), item_data(it), it->nbyte);
		memcpy(item_data(nit) + it->nbyte, data, nbyte);
		break;
	case REQ_IQPREPEND:
		memcpy(item_data(nit), data, nbyte);
		memcpy(item_data(nit) + nbyte, item_data(it), it->nbyte);
		break;
	default:
		break;
	}

	_item_store(nit, REQ_SET, c, true);
	_item_remove(nit);

	return IQ_LEASE;
}

item_co_result_t
_item_append_prepend_co(struct conn *c, struct item* it, char* data, size_t nbyte) {
	struct item* nit = NULL;
	size_t total_size;

	ASSERT (it != NULL && it->nbyte != 0);

	total_size = it->nbyte + nbyte;
	uint8_t id = item_slabid(it->nkey, total_size);

	nit = _item_alloc(id, item_key(it), it->nkey, it->dataflags, it->exptime, total_size, true, false);

	if (nit == NULL) {
		log_debug(LOG_VERB, "_iq_append_prepend_iq cannot allocate memory for item '%.*s'", it->nkey, item_key(it));
		return CO_ABORT;
	}

	switch (c->req_type) {
	case REQ_OQAPPEND:
		memcpy(item_data(nit), item_data(it), it->nbyte);
		memcpy(item_data(nit) + it->nbyte, data, nbyte);
		break;
	case REQ_OQPREPEND:
		memcpy(item_data(nit), data, nbyte);
		memcpy(item_data(nit) + nbyte, item_data(it), it->nbyte);
		break;
	default:
		break;
	}

	_item_store(nit, REQ_SET, c, true);
	_item_remove(nit);

	return CO_OK;
}

item_co_result_t
_item_oqincr_oqdecr(struct conn *c, char *key, size_t nkey, bool incr,
		long delta, char *sid, size_t nsid) {
	struct item* it = NULL;
	struct item* sess_it = NULL;
	struct item* pv_it = NULL;
	struct item* lease_it = NULL;
	struct item* colease_it = NULL;
	item_co_result_t ret = CO_OK;

	log_debug(LOG_VERB, "item_oqincr_oqdecr for sid '%.*s' key '%.*s'", nsid, sid, nkey, key);

	switch (c->req_type) {
	case REQ_OQINCR:
		stats_thread_incr(oqincr);
		break;
	case REQ_OQDECR:
		stats_thread_incr(oqdecr);
		break;
	default:
		break;
	}

	sess_it = _item_get(sid, nsid);
	if (sess_it != NULL) {
		if (sess_it->sess_status == ABORT) {
			_item_unlink(sess_it);
			item_unset_pinned(sess_it);
			_item_remove(sess_it);
			stats_thread_incr(sess_abort);
			return CO_ABORT;
		}
		_item_remove(sess_it);
		sess_it = NULL;
	}

	colease_it = _item_get_co_lease(key, nkey);
	if (colease_it != NULL) {
		ASSERT(item_has_co_lease(colease_it));
		if (item_has_c_lease(colease_it)) {
			abort_sessions(c, colease_it, sid, nsid);
			_item_unlink(colease_it);
			_item_remove(colease_it);
			colease_it = NULL;
		} else if (item_has_o_lease(colease_it)) {
			if (trig_check_keylist(item_data(colease_it), colease_it->nbyte, sid, nsid) != TRIG_OK) {	// same session
				// clean up session
				_item_remove(colease_it);
				clean_session(sid, nsid, c);
				return CO_ABORT;
			}
		}
	}

	it = _item_get(key, nkey);
	lease_it = _item_get_lease(key, nkey);

	if (lease_it == NULL || item_has_i_lease(lease_it)) {
		if (lease_it != NULL) {
			_item_unlink(lease_it);
			stats_thread_incr(qlease_voids);
		}

		// create a pending version of the key
		pv_it = _item_get_pending_version(key, nkey);
		if (pv_it == NULL && it != NULL) {
			ASSERT (it->nbyte != 0);
			pv_it = _item_create_pending_version(it);
			memcpy(item_data(pv_it), item_data(it), it->nbyte);
			_item_store(pv_it, REQ_SET, c, true);
		}
		ret = _item_add_delta_co(c, key, nkey, delta);
	}

	// associate the key with the sid
	if (colease_it == NULL) {
		_item_assoc_sid_colease(c, colease_it, key, nkey, sid, nsid, O_LEASE_REF);
	}

	sess_it = _item_get(sid, nsid);
	_item_assoc_key_sid(c, sess_it, key, nkey, sid, nsid);
	if (sess_it != NULL)
		_item_remove(sess_it);

	if (it != NULL && item_is_linked(it))
		_item_remove(it);

	if (pv_it != NULL && item_is_linked(pv_it))
		_item_remove(pv_it);

	if (lease_it != NULL)
		_item_remove(lease_it);

	if (colease_it != NULL)
		_item_remove(colease_it);

	return ret;
}

item_iq_result_t
_item_iqincr_iqdecr(struct conn *c, char *key, size_t nkey, bool incr,
		long delta, char *tid, size_t ntid, uint8_t *pending, uint64_t *new_lease_token) {
	struct item* it = NULL;
	struct item* trans_it = NULL;
	struct item* pv_it = NULL;
	struct item* lease_it = NULL;
	item_iq_result_t ret = IQ_LEASE;

	log_debug(LOG_VERB, "iq_incr_decr for tid '%.*s' key '%.*s'", ntid, tid, nkey, key);

	switch (c->req_type) {
	case REQ_IQINCR:
		stats_thread_incr(iqincr);
		break;
	case REQ_IQDECR:
		stats_thread_incr(iqdecr);
		break;
	default:
		break;
	}

	// get trans and it item
	it = _item_get(key, nkey);
	trans_it = _item_get(tid, ntid);
	lease_it = _item_get_lease(key, nkey);

	if (lease_it == NULL || item_has_i_lease(lease_it)) {
		if (lease_it != NULL) {
			_item_unlink(lease_it);

			stats_thread_incr(qlease_voids);
		}

		// create a new lease item
		lease_it = _item_create_lease(key, nkey, new_lease_token);
		item_set_q_inc_lease(lease_it);

		if (_item_store(lease_it, REQ_SET, c, true) == NOT_STORED) {
			log_debug(LOG_VERB, "cannot store lease item '%.*s'", lease_it->nkey, item_key(lease_it));
			ret = IQ_SERVER_ERROR;
		} else {
			stats_thread_incr(active_q_lease);
			stats_thread_incr(total_lease);
			stats_thread_incr(total_q_lease);

			// associate the key with the tid
			_item_assoc_key_tid(c, trans_it, key, nkey, tid, ntid);

			if (it != NULL) {
				ASSERT (it->nbyte != 0);

//				it->exptime = lease_it->exptime;

				// create a pending version of the key
				pv_it = _item_create_pending_version(it);
				pv_it->p = it->p;
				pv_it->exptime = lease_it->exptime;
				*pending = it->p;
				memcpy(item_data(pv_it), item_data(it), it->nbyte);
				_item_store(pv_it, REQ_SET, c, true);

				ret = _item_add_delta_iq(c, key, nkey, delta);
			} else {
				struct item* pending_it = _item_get_pending(key, nkey);
				if (pending_it != NULL) {
					*pending = 1;
					_item_remove(pending_it);
				}

				ret = IQ_LEASE_NO_VALUE;
			}
		}
	} else if (item_has_q_lease(lease_it)) {
		trig_keylen_t key_len = nkey;

		if (trans_it == NULL || trig_check_keylist(item_data(trans_it), trans_it->nbyte, key, key_len) == TRIG_NOTFOUND) {

			stats_thread_incr(qlease_aborts);

			ret = IQ_ABORT;
		} else {
			*new_lease_token = _item_lease_value(lease_it);

			// get the pending version of the key
			pv_it = _item_get_pending_version(key, nkey);

			if (pv_it == NULL) {	// no version of key but it is currently in the same session, so return NO_LEASE
				ret = IQ_LEASE_NO_VALUE;

				struct item* pending_it = _item_get_pending(key, nkey);
				if (pending_it != NULL) {
					*pending = 1;
					_item_remove(pending_it);
				}
			} else {
				*pending = pv_it->p;
				ret = _item_add_delta_iq(c, key, nkey, delta);
			}
		}
	}

	if (it != NULL)
		_item_remove(it);

	if (trans_it != NULL)
		_item_remove(trans_it);

	if (pv_it != NULL)
		_item_remove(pv_it);

	if (lease_it != NULL)
		_item_remove(lease_it);

	return ret;
}

void
_item_assoc_key_tid(struct conn* c, struct item* trans_it, char *key, size_t nkey, char *tid, size_t ntid) {
	u_int32_t old_keylistlen = 0;

	trig_keylen_t key_len = nkey;
	if (trans_it == NULL || trig_check_keylist(item_data(trans_it), trans_it->nbyte, key, key_len) == TRIG_NOTFOUND) {
		if (trans_it == NULL) {
			old_keylistlen = 0;
		} else {
			old_keylistlen = trans_it->nbyte;
		}

		u_int32_t num_of_bytes = old_keylistlen + trig_new_keylist_size(key, key_len);
		trig_listlen_t new_listlen = 0;

		// allocate memory for new item
		struct item* new_trans_it = _item_create_reserved_item(tid, ntid, num_of_bytes, true);
		item_set_sess(new_trans_it);

		if (trans_it != NULL) {
			trig_keylist_addkey(item_data(new_trans_it), &new_listlen, new_trans_it->nbyte,
					item_data(trans_it), trans_it->nbyte, key, nkey);

			item_unset_pinned(trans_it);
		} else {
			// There was no old keylist so create a new one with just the new key.
			trig_keylist_addkey(item_data(new_trans_it), &new_listlen, new_trans_it->nbyte,
					NULL, 0, key, nkey);

			stats_thread_incr(total_trans);
		}

		_item_store(new_trans_it, REQ_SET, c, true);

		_item_remove(new_trans_it);
	}
}

void
_item_assoc_key_sid(struct conn* c, struct item* sess_it, char *key, size_t nkey,
		char *sid, size_t nsid) {
	u_int32_t old_keylistlen = 0;

	trig_keylen_t key_len = nkey;
	if (sess_it == NULL || trig_check_keylist(item_data(sess_it), sess_it->nbyte, key, key_len) == TRIG_NOTFOUND) {
		if (sess_it == NULL) {
			old_keylistlen = 0;
		} else {
			old_keylistlen = sess_it->nbyte;
		}

		u_int32_t num_of_bytes = old_keylistlen + trig_new_keylist_size(key, key_len);
		trig_listlen_t new_listlen = 0;

		// allocate memory for new item
		struct item* new_trans_it = _item_create_reserved_item(sid, nsid, num_of_bytes, true);
		new_trans_it->sess_status = ALIVE;
		item_set_sess(new_trans_it);

		if (sess_it != NULL) {
			trig_keylist_addkey(item_data(new_trans_it), &new_listlen, new_trans_it->nbyte,
					item_data(sess_it), sess_it->nbyte, key, nkey);
			item_unset_pinned(sess_it);
		} else {
			// There was no old keylist so create a new one with just the new key.
			trig_keylist_addkey(item_data(new_trans_it), &new_listlen, new_trans_it->nbyte,
					NULL, 0, key, nkey);
		}

		_item_store(new_trans_it, REQ_SET, c, true);

		_item_remove(new_trans_it);
	}
}

/* associate sid with the colease item if sid has not been associated with this lease before */
void
_item_assoc_sid_colease(struct conn* c, struct item* colease_it, char* key, size_t nkey, char *sid, size_t nsid, item_coflags_t type) {
	u_int32_t old_keylistlen = 0;
	if (colease_it != NULL)
		old_keylistlen = colease_it->nbyte;

	trig_keylen_t key_len = nsid;

	// only associate sid with colease if it is not added before
	if (colease_it == NULL ||
			trig_check_keylist(item_data(colease_it), old_keylistlen, sid, key_len) == TRIG_NOTFOUND) {
		if (colease_it == NULL) {
			old_keylistlen = 0;
		} else {
			old_keylistlen = colease_it->nbyte;
		}

		u_int32_t num_of_bytes = old_keylistlen + trig_new_keylist_size(sid, key_len);
		trig_listlen_t new_listlen = 0;

		// allocate memory for new item
		size_t colease_nkey = nkey + PREFIX_KEY_LEN;
		char colease_key[colease_nkey];
		mc_get_co_lease_key(key, nkey, &colease_key);
		struct item* new_colease_it = _item_create_reserved_item(colease_key, colease_nkey, num_of_bytes, true);

		if (type == C_LEASE)
			item_set_c_lease(new_colease_it);
		else if (type == O_LEASE_INV)
			item_set_o_lease_inv(new_colease_it);
		else if (type == O_LEASE_REF)
			item_set_o_lease_ref(new_colease_it);

		if (colease_it != NULL) {
			// this should be guaranteed before calling this function
			ASSERT(colease_it->coflags == type);

			trig_keylist_addkey(item_data(new_colease_it), &new_listlen, new_colease_it->nbyte,
					item_data(colease_it), colease_it->nbyte, sid, key_len);
		} else {
			// There was no old keylist so create a new one with just the new key.
			trig_keylist_addkey(item_data(new_colease_it), &new_listlen, new_colease_it->nbyte,
					NULL, 0, sid, key_len);
		}

		_item_store(new_colease_it, REQ_SET, c, true);
		if (type == C_LEASE) {
			stats_thread_incr(active_c_lease);
			if (colease_it == NULL)
				stats_thread_incr(total_c_lease);
		} else if (type == O_LEASE_INV || type == O_LEASE_REF) {
			stats_thread_incr(active_o_lease);
			if (colease_it == NULL)
				stats_thread_incr(total_o_lease);
		}

		_item_remove(new_colease_it);
	}
}

void
_item_assoc_tid_ptrans(struct conn* c, struct item* ptrans_it,
		char* key, size_t nkey, char *tid, size_t ntid) {
	u_int32_t old_keylistlen = 0;
	if (ptrans_it != NULL)
		old_keylistlen = ptrans_it->nbyte;

	trig_keylen_t key_len = ntid;

	// only associate sid with colease if it is not added before
	if (ptrans_it == NULL ||
			trig_check_keylist(item_data(ptrans_it), old_keylistlen, tid, key_len) == TRIG_NOTFOUND) {
		if (ptrans_it == NULL) {
			old_keylistlen = 0;
		} else {
			old_keylistlen = ptrans_it->nbyte;
		}

		u_int32_t num_of_bytes = old_keylistlen + trig_new_keylist_size(tid, key_len);
		trig_listlen_t new_listlen = 0;

		// allocate memory for new item
		size_t ptrans_nkey = nkey + PREFIX_KEY_LEN;
		char ptrans_key[ptrans_nkey];
		mc_get_ptrans_key(key, nkey, &ptrans_key);
		struct item* new_ptrans_it = _item_create_reserved_item(ptrans_key, ptrans_nkey, num_of_bytes, true);
		new_ptrans_it->exptime = 0;
		item_set_ptrans(new_ptrans_it);

		if (ptrans_it != NULL) {
			trig_keylist_addkey(item_data(new_ptrans_it), &new_listlen, new_ptrans_it->nbyte,
					item_data(ptrans_it), ptrans_it->nbyte, tid, key_len);
		} else {
			// There was no old keylist so create a new one with just the new key.
			trig_keylist_addkey(item_data(new_ptrans_it), &new_listlen, new_ptrans_it->nbyte,
					NULL, 0, tid, key_len);
		}

		_item_store(new_ptrans_it, REQ_SET, c, true);

		_item_remove(new_ptrans_it);
	}
}

/* associate sid with the colease item if sid has not been associated with this lease before */
void
_item_assoc_tid_lease(struct conn* c, struct item* lease_it,
		char* key, size_t nkey, char *tid, size_t ntid) {
	u_int32_t old_keylistlen = 0;
	if (lease_it != NULL)
		old_keylistlen = lease_it->nbyte;

	trig_keylen_t key_len = ntid;

	// only associate sid with colease if it is not added before
	if (lease_it == NULL || trig_check_keylist(item_data(lease_it), old_keylistlen, tid, key_len) == TRIG_NOTFOUND) {
		if (lease_it == NULL) {
			old_keylistlen = 0;
		} else {
			old_keylistlen = lease_it->nbyte;
		}

		u_int32_t num_of_bytes = old_keylistlen + trig_new_keylist_size(tid, key_len);
		trig_listlen_t new_listlen = 0;

		// allocate memory for new item
		size_t lease_nkey = nkey + PREFIX_KEY_LEN;
		char lease_key[lease_nkey];
		mc_get_lease_key(key, nkey, &lease_key);
		struct item* new_lease_it = _item_create_reserved_item(lease_key, lease_nkey, num_of_bytes, true);
		item_set_q_inv_lease(new_lease_it);

		if (lease_it != NULL) {
			trig_keylist_addkey(item_data(new_lease_it), &new_listlen, new_lease_it->nbyte,
					item_data(lease_it), lease_it->nbyte, tid, key_len);
		} else {
			// There was no old keylist so create a new one with just the new key.
			trig_keylist_addkey(item_data(new_lease_it), &new_listlen, new_lease_it->nbyte,
					NULL, 0, tid, key_len);
		}

		_item_store(new_lease_it, REQ_SET, c, true);

		stats_thread_incr(active_q_lease);
		stats_thread_incr(total_q_lease);
		stats_thread_incr(total_lease);

		_item_remove(new_lease_it);
	}
}

void
_item_assoc_entry_to_list(struct conn* c, struct item* it, char *entry_id, size_t entry_nid,
		char *key, size_t nkey) {
	u_int32_t old_keylistlen = 0;

	trig_keylen_t key_len = entry_nid;
	if (it == NULL || trig_check_keylist(item_data(it), it->nbyte, entry_id, key_len) == TRIG_NOTFOUND) {
		if (it == NULL) {
			old_keylistlen = 0;
		} else {
			old_keylistlen = it->nbyte;
		}

		u_int32_t num_of_bytes = old_keylistlen + trig_new_keylist_size(entry_id, key_len);
		trig_listlen_t new_listlen = 0;

		// allocate memory for new item
		struct item* new_it = _item_create_reserved_item(key, nkey, num_of_bytes, true);

		if (it != NULL) {
			trig_keylist_addkey(item_data(new_it), &new_listlen, new_it->nbyte,
					item_data(it), it->nbyte, entry_id, key_len);
			item_unset_pinned(it);
		} else {
			// There was no old keylist so create a new one with just the new key.
			trig_keylist_addkey(item_data(new_it), &new_listlen, new_it->nbyte,
					NULL, 0, entry_id, key_len);
		}

		_item_store(new_it, REQ_SET, c, true);

		_item_remove(new_it);
	}
}

void
_item_remove_entry_from_list(struct conn *c, struct item* it, char* key, size_t nkey, char *entry_id, size_t entry_nid) {
	if (it == NULL)
		return;

	u_int32_t old_keylistlen = it->nbyte;

	trig_keylen_t key_len = entry_nid;
	u_int32_t num_of_bytes = old_keylistlen - trig_new_keylist_size(entry_id, key_len);
	trig_listlen_t new_listlen = 0;

	if (trig_check_keylist(item_data(it), it->nbyte, entry_id, key_len) == TRIG_OK) {
		if (it->nbyte == key_len + sizeof(trig_keylen_t)) {
			if (item_has_c_lease(it)) {
				stats_thread_incr(released_c_lease);
			} else if (item_has_o_lease(it)) {
				stats_thread_incr(released_o_lease);
			} else if (item_has_q_lease(it)) {
				stats_thread_incr(released_q_lease);
			}
			_item_unlink(it);
			return;
		}

		// allocate memory for new item
		struct item* new_it = _item_create_reserved_item(key, nkey, num_of_bytes, true);

		if (item_has_c_lease(it)) {
			new_it->coflags = it->coflags;
			item_set_c_lease(new_it);
		} else if (item_has_o_lease(it)) {
			new_it->coflags = it->coflags;
			//			item_set_o_lease_inv(new_it);
		} else if (item_has_q_lease(it)) {
			//			new_it->flags = it->flags;
			item_set_q_inv_lease(new_it);
		}

		trig_keylist_rmvkey(item_data(new_it), &new_listlen, new_it->nbyte,
				item_data(it), it->nbyte, entry_id, key_len);

		_item_store(new_it, REQ_SET, c, true);
		_item_remove(new_it);
	}
}

void
_item_remove_sid_colease(struct conn* c, struct item* colease_it, char* key, size_t nkey, char *sid, size_t nsid) {
	size_t colease_nkey = nkey + PREFIX_KEY_LEN;
	char colease_key[colease_nkey];
	mc_get_co_lease_key(key, nkey, &colease_key);

	_item_remove_entry_from_list(c, colease_it, colease_key, colease_nkey, sid, nsid);
}

void
_item_remove_tid_lease(struct conn* c, struct item* lease_it, char* key, size_t nkey, char *tid, size_t ntid) {
	size_t lease_nkey = nkey + PREFIX_KEY_LEN;
	char lease_key[lease_nkey];
	mc_get_lease_key(key, nkey, &lease_key);

	_item_remove_entry_from_list(c, lease_it, lease_key, lease_nkey, tid, ntid);
}

void
_item_remove_tid_ptrans(struct conn* c, struct item* ptrans_it, char* key, size_t nkey, char *tid, size_t ntid) {
	size_t ptrans_nkey = nkey + PREFIX_KEY_LEN;
	char ptrans_key[ptrans_nkey];
	mc_get_ptrans_key(key, nkey, &ptrans_key);

	if (ptrans_it == NULL)
		return;

	u_int32_t old_keylistlen = ptrans_it->nbyte;

	trig_keylen_t key_len = ntid;
	u_int32_t num_of_bytes = old_keylistlen - trig_new_keylist_size(tid, key_len);
	trig_listlen_t new_listlen = 0;

	if (trig_check_keylist(item_data(ptrans_it), ptrans_it->nbyte, tid, key_len) == TRIG_OK) {
		if (ptrans_it->nbyte == key_len + sizeof(trig_keylen_t)) {
			_item_unlink(ptrans_it);
			return;
		}

		// allocate memory for new item
		struct item* new_ptrans_it = _item_create_reserved_item(ptrans_key, ptrans_nkey, num_of_bytes, true);
		new_ptrans_it->exptime = 0;
		new_ptrans_it->coflags = ptrans_it->coflags;

		trig_keylist_rmvkey(item_data(new_ptrans_it), &new_listlen, new_ptrans_it->nbyte,
				item_data(ptrans_it), ptrans_it->nbyte, tid, key_len);

		_item_store(new_ptrans_it, REQ_SET, c, true);
		_item_remove(new_ptrans_it);
	}
}

item_iq_result_t
_item_add_delta_iq(struct conn* c, char*key, size_t nkey, long delta) {
	item_delta_result_t dret = DELTA_OK;
	char temp[KEY_MAX_LEN];
	size_t pv_nkey = nkey + PREFIX_KEY_LEN;
	char pv_key[pv_nkey];

	mc_get_version_key(key, nkey, &pv_key);

	switch (c->req_type) {
	case REQ_IQINCR:
		dret = _item_add_delta(c, pv_key, pv_nkey, true, delta, temp);
		break;
	case REQ_IQDECR:
		dret = _item_add_delta(c, pv_key, pv_nkey, false, delta, temp);
		break;
	default:
		break;
	}

	switch (dret) {
	case DELTA_EOM:
		return IQ_SERVER_ERROR;
	case DELTA_NON_NUMERIC:
		return IQ_CLIENT_ERROR;
	case DELTA_OK:
		return IQ_LEASE;
	case DELTA_NOT_FOUND:
		return IQ_ABORT;
	default:
		return IQ_ABORT;
	}
}

item_co_result_t
_item_add_delta_co(struct conn* c, char*key, size_t nkey, long delta) {
	item_delta_result_t dret = DELTA_OK;
	char temp[KEY_MAX_LEN];
	size_t pv_nkey = nkey + PREFIX_KEY_LEN;
	char pv_key[pv_nkey];

	mc_get_version_key(key, nkey, &pv_key);

	switch (c->req_type) {
	case REQ_OQINCR:
		dret = _item_add_delta(c, pv_key, pv_nkey, true, delta, temp);
		break;
	case REQ_OQDECR:
		dret = _item_add_delta(c, pv_key, pv_nkey, false, delta, temp);
		break;
	default:
		break;
	}

	switch (dret) {
	case DELTA_EOM:
		return CO_ABORT;
	case DELTA_NON_NUMERIC:
		return CO_ABORT;
	case DELTA_OK:
		return CO_OK;
	case DELTA_NOT_FOUND:
		return CO_OK;
	default:
		return CO_ABORT;
	}
}

item_store_result_t
item_get_and_delete(char* key, uint8_t nkey, struct conn *c, int delete_lease)
{
	struct item *it = NULL; /* item for this key */
	item_store_result_t found;
	struct item *lease_it = NULL, *pv_it = NULL;
	struct item *colease_it = NULL;

	log_debug(LOG_VERB, "get_and_delete for '%.*s'", nkey, key);

	pthread_mutex_lock(&cache_lock);

	if (delete_lease == 1) {
		lease_it = _item_get_lease(key, nkey);
		if (lease_it != NULL) {
			_item_unlink(lease_it);
			_item_remove(lease_it);
		}

		pv_it = _item_get_pending_version(key, nkey);
		if (pv_it != NULL) {
			_item_unlink(pv_it);
			_item_remove(pv_it);
		}

		colease_it = _item_get_co_lease(key, nkey);
		if (colease_it != NULL) {
			_item_unlink(colease_it);
			_item_remove(colease_it);
		}
	}

	it = _item_get(key, nkey);

	if (it != NULL) {
		found = EXISTS;

		_item_unlink(it);
		_item_remove(it);

		stats_thread_incr(delete_hit);
	} else {
		found = NOT_FOUND;
		stats_thread_incr(delete_miss);
	}

	pthread_mutex_unlock(&cache_lock);

	return found;
}

void
item_unset_pinned(struct item *it)
{
	item_release_refcount(it);
}


void
item_set_pinned(struct item *it)
{
	item_acquire_refcount(it);
}
