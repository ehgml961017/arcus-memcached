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
#include <assert.h>
#include <pthread.h>
#include <libpmemobj.h>

#include "pmem_engine.h"

#define GET_HASH_BUCKET(hash, mask) ((hash) & (mask))

static EXTENSION_LOGGER_DESCRIPTOR *logger;

ENGINE_ERROR_CODE pm_assoc_init(struct pmem_engine *engine)
{
    logger = engine->server.log->get_logger();

    TOID(struct pm_assoc) assoc = POBJ_ROOT(pop, struct pm_assoc);
    if(TOID_IS_NULL(D_RO(assoc)->hashtable)) {
	TX_BEGIN(pop){
            TX_ADD(assoc);

            D_RW(assoc)->hashsize = 1024 * 1024; 
            D_RW(assoc)->hashmask = D_RO(assoc)->hashsize-1;

            D_RW(assoc)->hashtable = TX_ALLOC(TOID(struct _hash_item), D_RO(assoc)->hashsize);
            if(TOID_IS_NULL(D_RO(assoc)->hashtable)) {
                return ENGINE_ENOMEM;
            }
        } TX_ONABORT {
            abort();
        } TX_END
    }

    engine->assoc = assoc;

    logger->log(EXTENSION_LOG_INFO, NULL, "PMEM ASSOC module initialized.\n");
    return ENGINE_SUCCESS;
}

/*
void pm_assoc_final(PMEMobjpool *pop, struct pmem_engine *engine)
{
    TX_BEGIN(pop) {
        TOID(struct pm_assoc) *assoc = &engine->assoc;
        TX_ADD(assoc);

	TOID(hash_item) *it;
        TX_ADD_FIELD(it, h_next);

	for(int ii=0; ii < D_RO(assoc)->hashsize; ++i) {
	    while ((it = D_RO(assoc)->hashtable[ii]) != NULL) {
	        D_RW(assoc)->hashtable[ii] = D_RO(it)->h_next;
		TX_FREE(it);
	    }
	}

	TX_FREE(D_RW(assoc)->hashtable);
    } TX_ONABORT {
        fprintf(stderr, "%s: transaction aborted: %s\n", __func__, pmemobj_errormsg());
    } TX_END
    logger->log(EXTENSION_LOG_INFO, NULL, "PMEM ASSOC module destroyed.\n");
}
*/

hash_item *pm_assoc_find(struct pmem_engine *engine, uint32_t hash,
                         const char *key, const size_t nkey)
{
    struct pm_assoc *assoc = D_RW(engine->assoc);
    hash_item *curr = NULL;

    uint32_t bucket = GET_HASH_BUCKET(hash, assoc->hashmask);

    while ((curr = D_RW(D_RW(assoc->hashtable)[bucket])) != NULL) {
        if (nkey == curr->nkey && hash == curr->hval &&
            memcmp(key, pm_item_get_key(curr), nkey) == 0)
            break;
        curr = curr->h_next;
    }

    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return curr;

}

/* Note: this isn't an assoc_update.  The key must not already exist to call this */
int pm_assoc_insert(struct pmem_engine *engine, uint32_t hash, hash_item *it)
{
    struct pm_assoc *assoc = D_RW(engine->assoc);
    uint32_t bucket = GET_HASH_BUCKET(hash, assoc->hashmask);
    TOID(struct _hash_item) toid_it;

    /* shouldn't have duplicately named things defined */
    assert(pm_assoc_find(engine, hash, pm_item_get_key(it), it->nkey) == 0);

    it->h_next = D_RW(D_RW(assoc->hashtable)[bucket]);
    
    TOID_ASSIGN(toid_it, pmemobj_oid((void*)it));
    D_RW(assoc->hashtable)[bucket] = toid_it;
    assert(D_RW(D_RW(assoc->hashtable)[bucket]) != NULL);
    assoc->hash_items++;

    MEMCACHED_ASSOC_INSERT(pm_item_get_key(it), it->nkey, assoc->hash_items);
    return 1;
}
	
/*
void pm_assoc_delete(struct pmem_engine *engine, uint32_t hash,
                     const char *key, const size_t nkey)
{
    hash_item *curr=NULL;
    hash_item *prev=NULL;
    struct pm_assoc *assoc = &engine->assoc;
    uint32_t bucket = GET_HASH_BUCKET(hash, assoc->hashmask);

    while ((curr = assoc->hashtable[bucket]) != NULL) {
        if (nkey == curr->nkey && hash == curr->hval &&
            memcmp(key, pm_item_get_key(curr), nkey) == 0)
            break;
        prev = curr;
        curr = curr->h_next;
    }
    if (curr != NULL) {
        if (prev == NULL)
            assoc->hashtable[bucket] = curr->h_next;
        else
            prev->h_next = curr->h_next;
        assoc->hash_items--; 
    }

}*/

void pm_assoc_cleanup(struct pmem_engine *engine)
{
    struct pm_assoc *assoc = D_RW(engine->assoc);
    hash_item *prev;
    hash_item *curr;
    hash_item *next;
    TOID(struct _hash_item) it;

    for(int ii=0; ii < assoc->hashsize; ++ii) {
        if((curr = D_RW(D_RW(assoc->hashtable)[ii])) != NULL) {
            prev = NULL;
	    while(curr != NULL) {
                if(curr->stored) {
                    printf("go next\n");
                    prev = curr;
                    curr = curr->h_next;
                } else {
                    printf("delete");
                    if(prev == NULL)
                        TOID_ASSIGN(D_RW(assoc->hashtable)[ii], pmemobj_oid((void*)curr->h_next));
		    else
                        prev->h_next = curr->h_next;
                    next = curr->h_next;
		    printf("curr : %d", curr->nkey);
		    TOID_ASSIGN(it, pmemobj_oid((void*)curr));
                    POBJ_FREE(&it);
		    curr = next;
                    assoc->hash_items--;
		}
	    }
	}
    }
}
