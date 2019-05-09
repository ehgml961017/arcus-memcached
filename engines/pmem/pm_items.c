/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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
#include "config.h"
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/time.h> /* gettimeofday() */
#include <libpmemobj.h>

#include "pmem_engine.h"

//#define SET_DELETE_NO_MERGE
//#define BTREE_DELETE_NO_MERGE

/* item unlink cause */
enum item_unlink_cause {
    ITEM_UNLINK_NORMAL = 1, /* unlink by normal request */
    ITEM_UNLINK_EVICT,      /* unlink by eviction */
    ITEM_UNLINK_INVALID,    /* unlink by invalidation such like expiration/flush */
    ITEM_UNLINK_REPLACE,    /* unlink by replacement of set/replace command,
                             * simple kv type only
                             */
    ITEM_UNLINK_ABORT,      /* unlink by abortion of creating a collection
                             * collection type only
                             */
    ITEM_UNLINK_EMPTY,      /* unlink by empty collection
                             * collection type only
                             */
    ITEM_UNLINK_STALE       /* unlink by staleness */
};

static EXTENSION_LOGGER_DESCRIPTOR *logger;

/*
#define POOL_PATH "/home/ehgml/pmem_hashtable/hashtable_pool_file.obj"
PMEMobjpool *pop;
*/

/*
 * Static functions
 */

#define ITEM_REFCOUNT_FULL 65535
#define ITEM_REFCOUNT_MOVE 32768

static inline void ITEM_REFCOUNT_INCR(hash_item *it)
{
    it->refcount++;
    if (it->refcount == ITEM_REFCOUNT_FULL) {
        it->refchunk += 1;
        it->refcount -= ITEM_REFCOUNT_MOVE;
        assert(it->refchunk != 0); /* overflow */
    }
}

static inline void ITEM_REFCOUNT_DECR(hash_item *it)
{
    it->refcount--;
    if (it->refcount == 0 && it->refchunk > 0) {
        it->refchunk -= 1;
        it->refcount = ITEM_REFCOUNT_MOVE;
    }
}

/* warning: don't use these macros with a function, as it evals its arg twice */
static inline size_t ITEM_ntotal(struct pmem_engine *engine, const hash_item *item)
{
    size_t ret;
    ret = sizeof(*item) + item->nkey + item->nbytes;
    if (engine->config.use_cas) {
        ret += sizeof(uint64_t);
    }
    return ret;
}

static inline size_t ITEM_stotal(struct pmem_engine *engine, const hash_item *item)
{
    size_t ntotal = ITEM_ntotal(engine, item);
    return ntotal;
}

/* Get the next CAS id for a new item. */
static uint64_t get_cas_id(void)
{
    static uint64_t cas_id = 0;
    return ++cas_id;
}

/* Enable this for reference-count debugging. */
#if 0
# define DEBUG_REFCNT(it,op) \
         fprintf(stderr, "item %x refcnt(%c) %d %c\n", \
                        it, op, it->refcount, \
                        (it->it_flags & ITEM_LINKED) ? 'L' : ' ')
#else
# define DEBUG_REFCNT(it,op) while(0)
#endif

static bool do_item_isvalid(struct pmem_engine *engine,
                            hash_item *it, rel_time_t current_time)
{
    /* check if it's expired */
    if (it->exptime != 0 && it->exptime <= current_time) {
        return false; /* expired */
    }
    /* check flushed items as well as expired items */
    if (engine->config.oldest_live != 0) {
        if (engine->config.oldest_live <= current_time &&
            it->time <= engine->config.oldest_live)
            return false; /* flushed by flush_all */
    }
    return true; /* Yes, it's a valid item */
}

static ENGINE_ERROR_CODE do_item_link(struct pmem_engine *engine, TOID(struct _hash_item) it)
{
    printf("nkey in do_item_link : %d\n", D_RO(it)->nkey);
    printf("item's address in do_item_link : %p\n", (void*)&it);
    const char *key = pm_item_get_key(D_RO(it));
    size_t stotal = ITEM_stotal(engine, D_RO(it));
    assert((D_RO(it)->iflag & ITEM_LINKED) == 0);
    assert(D_RO(it)->nbytes < (1024 * 1024));  /* 1MB max size */

    MEMCACHED_ITEM_LINK(key, D_RO(it)->nkey, D_RO(it)->nbytes);

    //TX_ADD(it);
    /* Allocate a new CAS ID on link. */
    pm_item_set_cas(D_RO(it), get_cas_id());

    /* link the item to the hash table */
    D_RW(it)->iflag |= ITEM_LINKED;
    D_RW(it)->time = engine->server.core->get_current_time();
    D_RW(it)->hval = engine->server.core->hash(key, D_RO(it)->nkey, 0);
    pm_assoc_insert(engine, D_RW(it)->hval, D_RW(it));

    /* update item statistics */
    pthread_mutex_lock(&engine->stats.lock);
    engine->stats.curr_bytes += stotal;
    engine->stats.curr_items += 1;
    engine->stats.total_items += 1;
    pthread_mutex_unlock(&engine->stats.lock);

    printf("success : do_item_link\n");

    return ENGINE_SUCCESS;
}

/*@null@*/
static hash_item *do_item_alloc(struct pmem_engine *engine,
                                const void *key, const size_t nkey,
                                const int flags, const rel_time_t exptime,
                                const int nbytes, const void *cookie)
{
//    pop = pmemobj_open(POOL_PATH, "HASHTABLE_POOL");
    assert(pop != NULL);
    TOID(struct _hash_item) it;
    size_t ntotal;
    rel_time_t real_exptime = exptime;

#ifdef ENABLE_STICKY_ITEM
    /* sticky memory limit check */
    if (real_exptime == (rel_time_t)(-1)) { /* sticky item */
        real_exptime = 0; // ignore sticky item */
    }
#endif

    ntotal = sizeof(hash_item) + nkey + nbytes;
    if (engine->config.use_cas) {
        ntotal += sizeof(uint64_t);
    }

    TX_BEGIN(pop) {
        it = TX_ALLOC(struct _hash_item, ntotal);
        if(TOID_IS_NULL(it)) {
            return NULL;
	}

        D_RW(it)->slabs_clsid = 1;
    //assert(it->slabs_clsid == 0);
        D_RW(it)->next = D_RW(it)->prev = D_RW(it); /* special meaning: unlinked from LRU */
        D_RW(it)->h_next = 0;
        D_RW(it)->refcount = 1;     /* the caller will have a reference */
        D_RW(it)->refchunk = 0;
        DEBUG_REFCNT(it, '*');
        D_RW(it)->iflag = engine->config.use_cas ? ITEM_WITH_CAS : 0;
        D_RW(it)->nkey = nkey;
        D_RW(it)->nbytes = nbytes;
        D_RW(it)->flags = flags;
        memcpy((void*)pm_item_get_key(D_RO(it)), key, nkey);
        D_RW(it)->exptime = real_exptime;
        D_RW(it)->nprefix = 0;
        D_RW(it)->stored = false;
       // do_item_link(engine, it);
    } TX_ONABORT{
        abort();
    } TX_END

    printf("success : do_item_alloc\n");

    return D_RW(it);
}

//static void do_item_free(struct pmem_engine *engine, TOID(hash_item) it)
//{
//    assert((D_RO(it)->iflag & ITEM_LINKED) == 0);
//    assert(D_RO(it)->refcount == 0);

//    TX_BEGIN(pop) {
//        TX_ADD(it);
//        D_RW(it)->slabs_clsid = 0;
//        DEBUG_REFCNT(it, 'F');
//        TX_FREE(it);
//    } TX_ONABORT {
//        abort();
//    } TX_END
//}

//static void do_item_unlink(struct pmem_engine *engine, TOID(hash_item) it,
//                           enum item_unlink_cause cause)
//{
//    /* cause: item unlink cause will be used, later
//    */
//    const char *key = pm_item_get_key(D_RO(it));
//    size_t stotal = ITEM_stotal(engine, D_RO(it));
//    MEMCACHED_ITEM_UNLINK(key, D_RW(it)->nkey, D_RW(it)->nbytes);

//    if ((D_RO(it)->iflag & ITEM_LINKED) != 0) {
        /* unlink the item from hash table */
//        pm_assoc_delete(engine, D_RO(it)->hval, key, D_RO(it)->nkey);
//        D_RW(it)->iflag &= ~ITEM_LINKED;

        /* update item statistics */
//        pthread_mutex_lock(&engine->stats.lock);
//        engine->stats.curr_bytes -= stotal;
//        engine->stats.curr_items -= 1;
//        pthread_mutex_unlock(&engine->stats.lock);

        /* free the item if no one reference it */
//        if (D_RO(it)->refcount == 0) {
//            do_item_free(engine, it);
//        }
//    }
//}

static void do_item_release(struct pmem_engine *engine, hash_item *it)
{
    MEMCACHED_ITEM_REMOVE(pm_item_get_key(it), it->nkey, it->nbytes);
    /*FIXME need to error check*/
    if (it->refcount != 0) {
        ITEM_REFCOUNT_DECR(it);
        DEBUG_REFCNT(it, '-');
    }
    if (it->refcount == 0) {
        if ((it->iflag & ITEM_LINKED) == 0) {
            //do_item_free(engine, it);
        }
    }
}

static void do_item_replace(struct pmem_engine *engine,
                            hash_item *it, hash_item *new_it)
{
    MEMCACHED_ITEM_REPLACE(pm_item_get_key(it), it->nkey, it->nbytes,
                           pm_item_get_key(new_it), new_it->nkey, new_it->nbytes);
    //do_item_unlink(engine, it, ITEM_UNLINK_REPLACE);
    /* Cache item replacement does not drop the prefix item even if it's empty.
     * So, the below do_item_link function always return SUCCESS.
     */
    //(void)do_item_link(engine, new_it);
}

static hash_item *do_item_get(struct pmem_engine *engine,
                              const char *key, const size_t nkey,
                              bool LRU_reposition)
{
    rel_time_t current_time = engine->server.core->get_current_time();
    hash_item *it = pm_assoc_find(engine, engine->server.core->hash(key, nkey, 0),
                                  key, nkey);

    if (it != NULL) {
        if (do_item_isvalid(engine, it, current_time)==false) {
            //do_item_unlink(engine, it, ITEM_UNLINK_INVALID);
            it = NULL;
        }
    }
    if (it != NULL) {
        ITEM_REFCOUNT_INCR(it);
        DEBUG_REFCNT(it, '+');
    }

    if (engine->config.verbose > 2) {
        if (it == NULL) {
            logger->log(EXTENSION_LOG_INFO, NULL, "> NOT FOUND %s\n",
                        key);
        } else {
            logger->log(EXTENSION_LOG_INFO, NULL, "> FOUND KEY %s\n",
                        (const char*)pm_item_get_key(it));
        }
    }
    return it;
}

/*
 * Stores an item in the cache according to the semantics of one of the set
 * commands. In threaded mode, this is protected by the cache lock.
 *
 * Returns the state of storage.
 */
static ENGINE_ERROR_CODE do_item_store(struct pmem_engine *engine,
                                       hash_item *it, uint64_t *cas,
                                       ENGINE_STORE_OPERATION operation,
                                       const void *cookie)
{ 
    const char *key = pm_item_get_key(it);
    hash_item *old_it;
    hash_item *new_it = NULL;
    TOID(struct _hash_item) toid_it;
    ENGINE_ERROR_CODE stored;

    old_it = do_item_get(engine, key, it->nkey, true);

    if (old_it != NULL) {
        if (operation == OPERATION_ADD) {
            do_item_release(engine, old_it);
            return ENGINE_NOT_STORED;
        }
    } else {
        if (operation == OPERATION_REPLACE ||
            operation == OPERATION_APPEND || operation == OPERATION_PREPEND) {
            return ENGINE_NOT_STORED;
        }
        if (operation == OPERATION_CAS) {
            return ENGINE_KEY_ENOENT;
        }
    }

    stored = ENGINE_NOT_STORED;

    if (operation == OPERATION_CAS) {
        assert(old_it != NULL);
        if (pm_item_get_cas(it) == pm_item_get_cas(old_it)) {
            // cas validates
            // it and old_it may belong to different classes.
            // I'm updating the stats for the one that's getting pushed out
            do_item_replace(engine, old_it, it);
            stored = ENGINE_SUCCESS;
        } else {
            if (engine->config.verbose > 1) {
                logger->log(EXTENSION_LOG_WARNING, NULL,
                        "CAS:  failure: expected %"PRIu64", got %"PRIu64"\n",
                        pm_item_get_cas(old_it), pm_item_get_cas(it));
            }
            stored = ENGINE_KEY_EEXISTS;
        }
    } else {
        /*
         * Append - combine new and old record into single one. Here it's
         * atomic and thread-safe.
         */
        if (operation == OPERATION_APPEND || operation == OPERATION_PREPEND) {
            assert(old_it != NULL);
            /*
             * Validate CAS
             */
            if (pm_item_get_cas(it) != 0) {
                // CAS much be equal
                if (pm_item_get_cas(it) != pm_item_get_cas(old_it)) {
                    stored = ENGINE_KEY_EEXISTS;
                }
            }
            if (stored == ENGINE_NOT_STORED) {
                /* we have it and old_it here - alloc memory to hold both */
                new_it = do_item_alloc(engine, key, it->nkey,
                                       old_it->flags,
                                       old_it->exptime,
                                       it->nbytes + old_it->nbytes - 2 /* CRLF */,
                                       cookie);
                if (new_it == NULL) {
                    /* SERVER_ERROR out of memory */
                    if (old_it != NULL)
                        do_item_release(engine, old_it);
                    return ENGINE_NOT_STORED;
                }

                /* copy data from it and old_it to new_it */
                if (operation == OPERATION_APPEND) {
                    memcpy(pm_item_get_data(new_it), pm_item_get_data(old_it), old_it->nbytes);
                    memcpy(pm_item_get_data(new_it) + old_it->nbytes - 2 /* CRLF */,
                           pm_item_get_data(it), it->nbytes);
                } else {
                    /* OPERATION_PREPEND */
                    memcpy(pm_item_get_data(new_it), pm_item_get_data(it), it->nbytes);
                    memcpy(pm_item_get_data(new_it) + it->nbytes - 2 /* CRLF */,
                           pm_item_get_data(old_it), old_it->nbytes);
                }

                it = new_it;
            }
        }
        if (stored == ENGINE_NOT_STORED) {
            if (old_it != NULL) {
                do_item_replace(engine, old_it, it);
                stored = ENGINE_SUCCESS;
            } else {
                /*convert it to TOID it*/
                printf("item's address in do_item_store : %p\n", (void*)&it);
                TOID_ASSIGN(toid_it, pmemobj_oid((void*)it));
                stored = do_item_link(engine, toid_it);
            }
            if (stored == ENGINE_SUCCESS) {
                *cas = pm_item_get_cas(it);
            }
        }
    }

    if (old_it != NULL) {
        do_item_release(engine, old_it);         /* release our reference */
    }
    if (new_it != NULL) {
        do_item_release(engine, new_it);
    }
    if (stored == ENGINE_SUCCESS) {
        *cas = pm_item_get_cas(it);
    }
    return stored;
}

static ENGINE_ERROR_CODE do_item_delete(struct pmem_engine *engine,
                                        const void* key, const size_t nkey,
                                        uint64_t cas)
{
    ENGINE_ERROR_CODE ret;
    hash_item *it = do_item_get(engine, key, nkey, true);
    if (it == NULL) {
        ret = ENGINE_KEY_ENOENT;
    } else {
        if (cas == 0 || cas == pm_item_get_cas(it)) {
            //do_item_unlink(engine, it, ITEM_UNLINK_NORMAL);
            ret = ENGINE_SUCCESS;
        } else {
            ret = ENGINE_KEY_EEXISTS;
        }
        do_item_release(engine, it);
    }
    return ret;
}

/********************************* ITEM ACCESS *******************************/

/*
 * Allocates a new item.
 */
hash_item *pm_item_alloc(struct pmem_engine *engine,
                      const void *key, size_t nkey, int flags,
                      rel_time_t exptime, int nbytes, const void *cookie)
{
    hash_item *it;
    pthread_mutex_lock(&engine->cache_lock);
    it = do_item_alloc(engine, key, nkey, flags, exptime, nbytes, cookie);
    pthread_mutex_unlock(&engine->cache_lock);
    return it;
}

/*
 * Returns an item if it hasn't been marked as expired,
 * lazy-expiring as needed.
 */
hash_item *pm_item_get(struct pmem_engine *engine, const void *key, const size_t nkey)
{
    hash_item *it;
    pthread_mutex_lock(&engine->cache_lock);
    it = do_item_get(engine, key, nkey, true);
    pthread_mutex_unlock(&engine->cache_lock);
    return it;
}

/*
 * Decrements the reference count on an item and adds it to the freelist if
 * needed.
 */
void pm_item_release(struct pmem_engine *engine, hash_item *item)
{
    pthread_mutex_lock(&engine->cache_lock);
    do_item_release(engine, item);
    pthread_mutex_unlock(&engine->cache_lock);
}

/*
 * Stores an item in the cache (high level, obeys set/add/replace semantics)
 */
ENGINE_ERROR_CODE pm_item_store(struct pmem_engine *engine,
                             hash_item *item, uint64_t *cas,
                             ENGINE_STORE_OPERATION operation,
                             const void *cookie)
{
    ENGINE_ERROR_CODE ret;

    pthread_mutex_lock(&engine->cache_lock);
    ret = do_item_store(engine, item, cas, operation, cookie);
    pthread_mutex_unlock(&engine->cache_lock);
    return ret;
}

ENGINE_ERROR_CODE pm_item_arithmetic(struct pmem_engine *engine,
                             const void* cookie,
                             const void* key,
                             const int nkey,
                             const bool increment,
                             const bool create,
                             const uint64_t delta,
                             const uint64_t initial,
                             const int flags,
                             const rel_time_t exptime,
                             uint64_t *cas,
                             uint64_t *result)
{
    return ENGINE_ENOTSUP;
}

/*
 * Delete an item.
 */

ENGINE_ERROR_CODE pm_item_delete(struct pmem_engine *engine,
                              const void* key, const size_t nkey,
                              uint64_t cas)
{
    ENGINE_ERROR_CODE ret;

    pthread_mutex_lock(&engine->cache_lock);
    ret = do_item_delete(engine, key, nkey, cas);
    pthread_mutex_unlock(&engine->cache_lock);
    return ret;
}

/*
 * Flushes expired items after a flush_all call
 */

ENGINE_ERROR_CODE pm_item_flush_expired(struct pmem_engine *engine,
                                        const char *prefix, const int nprefix,
                                        time_t when, const void* cookie)
{
    return ENGINE_ENOTSUP;

}

void pm_item_stats(struct pmem_engine *engine,
                   ADD_STAT add_stat, const void *cookie)
{
    return;
}

void pm_item_stats_sizes(struct pmem_engine *engine,
                      ADD_STAT add_stat, const void *cookie)
{
    return;
}

void pm_item_stats_reset(struct pmem_engine *engine)
{
    return;
}

ENGINE_ERROR_CODE pm_item_init(struct pmem_engine *engine)
{
    logger = engine->server.log->get_logger();
    logger->log(EXTENSION_LOG_INFO, NULL, "PMEM ITEM module initialized.\n");
    return ENGINE_SUCCESS;
}

void pm_item_final(struct pmem_engine *engine)
{
    logger->log(EXTENSION_LOG_INFO, NULL, "PMEM ITEM module destroyed.\n");
}

/*
 * Item access functions
 */
uint64_t pm_item_get_cas(const hash_item* item)
{
    if (item->iflag & ITEM_WITH_CAS) {
        return *(uint64_t*)(item + 1);
    }
    return 0;
}

void pm_item_set_cas(const hash_item* item, uint64_t val)
{
    if (item->iflag & ITEM_WITH_CAS) {
        *(uint64_t*)(item + 1) = val;
    }
}

const void* pm_item_get_key(const hash_item* item)
{
    char *ret = (void*)(item + 1);
    printf("nkey in pm_item_get_key : %d\n", item->nkey);
    printf("iflag in pm_item_get_key : %d\n", item->iflag);
    printf("item's address in pm_item_get_key : %p\n", (void*)&item);
    if (item->iflag & ITEM_WITH_CAS) {
        ret += sizeof(uint64_t);
    }
    return ret;
}

char* pm_item_get_data(const hash_item* item)
{
    return ((char*)pm_item_get_key(item)) + item->nkey;
}

uint8_t pm_item_get_clsid(const hash_item* item)
{
    return 0;
}
