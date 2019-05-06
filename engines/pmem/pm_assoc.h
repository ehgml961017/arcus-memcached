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
#ifndef PM_ASSOC_H
#define PM_ASSOC_H

#ifndef HASHMAP_TX_OFFSET
#define HASHMAP_TX_OFFSET 1004
#endif


TOID_DECLARE(TOID(struct _hash_item), HASHMAP_TX_OFFSET + 3);
TOID_DECLARE(struct pm_assoc, HASHMAP_TX_OFFSET + 2);
#define TOID_ARRAY(x) TOID(x)

struct pm_assoc {
   uint32_t hashsize;  /* hash table size */
   uint32_t hashmask;  /* hash bucket mask */

   /* cache item hash table : an array of hash tables */
   //hash_item **hashtable;
   TOID_ARRAY(TOID(struct _hash_item)) hashtable;

   /* Number of items in the hash table. */
   uint64_t hash_items;
};

/* associative array */
ENGINE_ERROR_CODE pm_assoc_init(struct pmem_engine *engine);
void              pm_assoc_final(struct pmem_engine *engine);

hash_item *       pm_assoc_find(struct pmem_engine *engine, uint32_t hash,
                             const char *key, const size_t nkey);
int               pm_assoc_insert(struct pmem_engine *engine, uint32_t hash, hash_item *item);
void              pm_assoc_delete(struct pmem_engine *engine, uint32_t hash,
                               const char *key, const size_t nkey);
#endif
