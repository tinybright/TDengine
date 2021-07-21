/*
 * Copyright (c) 2019 TAOS Data, Inc. <cli@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TDENGINE_CACHE_SLAB_H
#define TDENGINE_CACHE_SLAB_H

#include <stdlib.h> // for size_t
#include "cacheMutex.h"
#include "cacheTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cache_lru_list_t {
  CACHE_LRU_HOT   = 0,
  CACHE_LRU_WARM  = 64,
  CACHE_LRU_COLD  = 128,
} cache_lru_list_t;

struct cacheSlabClass {
  unsigned int size;        /* sizes of items */
  unsigned int perSlab;     /* how many items per slab */

  cacheMutex mutex;

  uint32_t    id;           /* slab class id */

  cacheItem *freeItem;      /* list of free item ptrs */
  unsigned int nFree;       /* free item count */
  unsigned int nAllocSlabs; /* how many slabs were allocated for this class */

  void **slabArray;         /* array of slab pointers */
  unsigned int nArray;      /* size of slab array */
};

cache_code_t cacheSlabInit(cache_t *);

uint32_t slabClsId(cache_t *cache, size_t size);

cacheItem* cacheSlabAllocItem(cache_t *cache, size_t ntotal, uint32_t id);

void cacheSlabFreeItem(cache_t *cache, cacheItem* item);

#ifdef __cplusplus
}
#endif

#endif  // TDENGINE_CACHE_SLAB_H