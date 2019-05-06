/*
 * arcus-memcached - Arcus memory cache server
 * Copyright 2016 JaM2in Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef PM_ITEMS_H
#define PM_ITEMS_H

#include <libpmemobj.h>

#ifndef HASHMAP_TX_OFFSET
#define HASHMAP_TX_OFFSET 1004
#endif

TOID_DECLARE(struct _hash_item, HASHMAP_TX_OFFSET + 1);

/* hash item strtucture */
typedef struct _hash_item {
    uint16_t refcount;  /* reference count */
    uint8_t  slabs_clsid;/* which slab class we're in */
    uint8_t  refchunk;  /* reference chunk */
    uint32_t flags;     /* Flags associated with the item (in network byte order) */
    struct _hash_item *next;   /* LRU chain next */
    struct _hash_item *prev;   /* LRU chain prev */
    struct _hash_item *h_next; /* hash chain next */
    rel_time_t time;    /* least recent access */
    rel_time_t exptime; /* When the item will expire (relative to process startup) */
    uint16_t iflag;     /* Intermal flags.
                         * Lower 8 bits are reserved for the core server,
                         * Upper 8 bits are reserved for engine implementation.
                         */
    uint16_t nkey;      /* The total length of the key (in bytes) */
    uint16_t nprefix;   /* The prefix length of the key (in bytes) */
    uint16_t dummy16;
    uint32_t hval;      /* hash value */
    uint32_t nbytes;    /* The total length of the data (in bytes) */
    bool stored;        /* Checking that this item is really stored */
} hash_item;

/* Item Internal Flags */
#define ITEM_WITH_CAS    1
#define ITEM_LINKED  (1<<8)
#define ITEM_SLABBED (2<<8)  /* NOT USED */

/*
 * You should not try to aquire any of the item locks before calling these
 * functions.
 */

/**
 * Allocate and initialize a new item structure
 * @param engine handle to the storage engine
 * @param key the key for the new item
 * @param nkey the number of bytes in the key
 * @param flags the flags in the new item
 * @param exptime when the object should expire
 * @param nbytes the number of bytes in the body for the item
 * @return a pointer to an item on success NULL otherwise
 */
hash_item *pm_item_alloc(struct pmem_engine *engine,
                      const void *key, size_t nkey, int flags,
                      rel_time_t exptime, int nbytes, const void *cookie);

/**
 * Get an item from the cache
 *
 * @param engine handle to the storage engine
 * @param key the key for the item to get
 * @param nkey the number of bytes in the key
 * @return pointer to the item if it exists or NULL otherwise
 */
hash_item *pm_item_get(struct pmem_engine *engine,
                       const void *key, const size_t nkey);

/**
 * Reset the item statistics
 * @param engine handle to the storage engine
 */
void pm_item_stats_reset(struct pmem_engine *engine);

/**
 * Get item statitistics
 * @param engine handle to the storage engine
 * @param add_stat callback provided by the core used to
 *                 push statistics into the response
 * @param cookie cookie provided by the core to identify the client
 */
void pm_item_stats(struct pmem_engine *engine, ADD_STAT add_stat, const void *cookie);

/**
 * Get detaild item statitistics
 * @param engine handle to the storage engine
 * @param add_stat callback provided by the core used to
 *                 push statistics into the response
 * @param cookie cookie provided by the core to identify the client
 */
void pm_item_stats_sizes(struct pmem_engine *engine, ADD_STAT add_stat, const void *cookie);

/**
 * Dump items from the cache
 * @param engine handle to the storage engine
 * @param slabs_clsid the slab class to get items from
 * @param limit the maximum number of items to receive
 * @param bytes the number of bytes in the return message (OUT)
 * @return pointer to a string containint the data
 *
 * @todo we need to rewrite this to use callbacks!!!! currently disabled
 */
char *pm_item_cachedump(struct pmem_engine *engine, const unsigned int slabs_clsid,
                     const unsigned int limit, const bool forward, const bool sticky,
                     unsigned int *bytes);

/**
 * Flush expired items from the cache
 * @param engine handle to the storage engine
 * @prefix prefix string
 * @nprefix prefix string length: -1(all prefixes), 0(null prefix)
 * @param when when the items should be flushed
 */
ENGINE_ERROR_CODE pm_item_flush_expired(struct pmem_engine *engine,
                                     const char *prefix, const int nprefix,
                                     time_t when, const void* cookie);

/**
 * Release our reference to the current item
 * @param engine handle to the storage engine
 * @param it the item to release
 */
void pm_item_release(struct pmem_engine *engine, hash_item *it);

/**
 * Store an item in the cache
 * @param engine handle to the storage engine
 * @param item the item to store
 * @param cas the cas value (OUT)
 * @param operation what kind of store operation is this (ADD/SET etc)
 * @return ENGINE_SUCCESS on success
 *
 * @todo should we refactor this into hash_item ** and remove the cas
 *       there so that we can get it from the item instead?
 */
ENGINE_ERROR_CODE pm_item_store(struct pmem_engine *engine, hash_item *item,
                             uint64_t *cas, ENGINE_STORE_OPERATION operation,
                             const void *cookie);

ENGINE_ERROR_CODE pm_item_arithmetic(struct pmem_engine *engine, const void* cookie,
                             const void* key, const int nkey, const bool increment,
                             const bool create, const uint64_t delta, const uint64_t initial,
                             const int flags, const rel_time_t exptime, uint64_t *cas,
                             uint64_t *result);

/**
 * Delete an item of the given key.
 * @param engine handle to the storage engine
 * @param key the key to delete
 * @param nkey the number of bytes in the key
 * @param cas the cas value
 */
ENGINE_ERROR_CODE pm_item_delete(struct pmem_engine *engine,
                              const void* key, const size_t nkey,
                              uint64_t cas);

ENGINE_ERROR_CODE pm_item_init(struct pmem_engine *engine);

void              pm_item_final(struct pmem_engine *engine);

/*
 * Item access functions
 */
uint64_t    pm_item_get_cas(const hash_item *item);
void        pm_item_set_cas(const hash_item* item, uint64_t val);
const void* pm_item_get_key(const hash_item *item);
char*       pm_item_get_data(const hash_item *item);
uint8_t     pm_item_get_clsid(const hash_item *item);

#endif
