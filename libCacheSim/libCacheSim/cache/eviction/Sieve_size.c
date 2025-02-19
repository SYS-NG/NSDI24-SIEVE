

#include "../../dataStructure/hashtable/hashtable.h"
#include "../../include/libCacheSim/cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;

  cache_obj_t *pointer;
  int          eviction_window_size;
} Sieve_size_params_t;

// ***********************************************************************
// ****                                                               ****
// ****                   function declarations                       ****
// ****                                                               ****
// ***********************************************************************
static void Sieve_size_free(cache_t *cache);
static bool Sieve_size_get(cache_t *cache, const request_t *req);
static cache_obj_t *Sieve_size_find(cache_t *cache, const request_t *req,
                               const bool update_cache);
static cache_obj_t *Sieve_size_insert(cache_t *cache, const request_t *req);
static cache_obj_t *Sieve_size_to_evict(cache_t *cache, const request_t *req);
static void Sieve_size_evict(cache_t *cache, const request_t *req);
static bool Sieve_size_remove(cache_t *cache, const obj_id_t obj_id);

// ***********************************************************************
// ****                                                               ****
// ****                   end user facing functions                   ****
// ****                                                               ****
// ****                       init, free, get                         ****
// ***********************************************************************

/**
 * @brief initialize cache
 *
 * @param ccache_params some common cache parameters
 * @param cache_specific_params cache specific parameters, see parse_params
 * function or use -e "print" with the cachesim binary
 */
cache_t *Sieve_size_init(const common_cache_params_t ccache_params,
                    const char *cache_specific_params) {
  cache_t *cache =
      cache_struct_init("Sieve_size", ccache_params, cache_specific_params);
  cache->cache_init = Sieve_size_init;
  cache->cache_free = Sieve_size_free;
  cache->get = Sieve_size_get;
  cache->find = Sieve_size_find;
  cache->insert = Sieve_size_insert;
  cache->evict = Sieve_size_evict;
  cache->remove = Sieve_size_remove;
  cache->to_evict = Sieve_size_to_evict;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 1;
  } else {
    cache->obj_md_size = 0;
  }

  cache->eviction_params = my_malloc(Sieve_size_params_t);
  memset(cache->eviction_params, 0, sizeof(Sieve_size_params_t));
  Sieve_size_params_t *params = (Sieve_size_params_t *)cache->eviction_params;
  params->pointer = NULL;
  params->q_head  = NULL;
  params->q_tail  = NULL;
  params->eviction_window_size = 100; 
  return cache;
}

/**
 * free resources used by this cache
 *
 * @param cache
 */
static void Sieve_size_free(cache_t *cache) {
  free(cache->eviction_params);
  cache_struct_free(cache);
}

/**
 * @brief this function is the user facing API
 * it performs the following logic
 *
 * ```
 * if obj in cache:
 *    update_metadata
 *    return true
 * else:
 *    if cache does not have enough space:
 *        evict until it has space to insert
 *    insert the object
 *    return false
 * ```
 *
 * @param cache
 * @param req
 * @return true if cache hit, false if cache miss
 */

static bool Sieve_size_get(cache_t *cache, const request_t *req) {
  bool ck_hit = cache_get_base(cache, req);
  return ck_hit;
}

// ***********************************************************************
// ****                                                               ****
// ****       developer facing APIs (used by cache developer)         ****
// ****                                                               ****
// ***********************************************************************

/**
 * @brief find an object in the cache
 *
 * @param cache
 * @param req
 * @param update_cache whether to update the cache,
 *  if true, the object is promoted
 *  and if the object is expired, it is removed from the cache
 * @return the object or NULL if not found
 */
static cache_obj_t *Sieve_size_find(cache_t *cache, const request_t *req,
                               const bool update_cache) {
  cache_obj_t *cache_obj = cache_find_base(cache, req, update_cache);
  if (cache_obj != NULL && update_cache) {
    cache_obj->sieve_size.freq = 1;
  }

  return cache_obj;
}

/**
 * @brief insert an object into the cache,
 * update the hash table and cache metadata
 * this function assumes the cache has enough space
 * eviction should be
 * performed before calling this function
 *
 * @param cache
 * @param req
 * @return the inserted object
 */
static cache_obj_t *Sieve_size_insert(cache_t *cache, const request_t *req) {
  Sieve_size_params_t *params = cache->eviction_params;
  cache_obj_t *obj = cache_insert_base(cache, req);
  prepend_obj_to_head(&params->q_head, &params->q_tail, obj);
  obj->sieve_size.freq = 0;

  return obj;
}

/**
 * @brief find the object to be evicted
 * this function does not actually evict the object or update metadata
 * not all eviction algorithms support this function
 * because the eviction logic cannot be decoupled from finding eviction
 * candidate, so use assert(false) if you cannot support this function
 *
 * @param cache the cache
 * @return the object to be evicted
 */
static cache_obj_t *Sieve_size_to_evict_with_freq(cache_t *cache,
                                             const request_t *req,
                                             int to_evict_freq) {
  Sieve_size_params_t *params = cache->eviction_params;
  cache_obj_t *pointer = params->pointer;

  /* if we have run one full around or first eviction */
  if (pointer == NULL) pointer = params->q_tail;

  /* find the first untouched */
  while (pointer != NULL && pointer->sieve_size.freq > to_evict_freq) {
    pointer = pointer->queue.prev;
  }

  /* if we have finished one around, start from the tail */
  if (pointer == NULL) {
    pointer = params->q_tail;
    while (pointer != NULL && pointer->sieve_size.freq > to_evict_freq) {
      pointer = pointer->queue.prev;
    }
  }

  if (pointer == NULL) return NULL;

  return pointer;
}

static cache_obj_t *Sieve_size_evict_largest_from_window(cache_t *cache, cache_obj_t *first_obj_in_window, int to_evict_freq) {
  Sieve_size_params_t *params     = cache->eviction_params;
  cache_obj_t *pointer            = first_obj_in_window;
  cache_obj_t *eviction_candidate = first_obj_in_window;
  int         window_remainder    = params->eviction_window_size;
  

  while (window_remainder-- > 0) {

    /* if we have run one full around or first eviction */
    if (pointer == NULL) pointer = params->q_tail;

    if (eviction_candidate == NULL || pointer->obj_size > eviction_candidate->obj_size) {
      eviction_candidate = pointer;
    }
    
    // // Only consider objects whose frequency is <= the threshold.
    // if (pointer->sieve_size.freq <= to_evict_freq) {
    //   // Choose the object with the largest size.
    //   if (eviction_candidate == NULL || pointer->obj_size > eviction_candidate->obj_size) {
    //     eviction_candidate = pointer;
    //   }
    // }
    // Move to the next object in the list.
    pointer = pointer->queue.prev;
  }

  return eviction_candidate;
}


static cache_obj_t *Sieve_size_to_evict(cache_t *cache, const request_t *req) {
  // because we do not change the frequency of the object,
  // if all objects have frequency 1, we may return NULL
  int to_evict_freq = 0;

  cache_obj_t *obj_to_evict =
      Sieve_size_to_evict_with_freq(cache, req, to_evict_freq);

  while (obj_to_evict == NULL) {
    to_evict_freq += 1;

    obj_to_evict = Sieve_size_to_evict_with_freq(cache, req, to_evict_freq);
  }

  obj_to_evict = Sieve_size_evict_largest_from_window(cache, obj_to_evict, to_evict_freq);

  return obj_to_evict;
}

/**
 * @brief evict an object from the cache
 * it needs to call cache_evict_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param req not used
 * @param evicted_obj if not NULL, return the evicted object to caller
 */
static void Sieve_size_evict(cache_t *cache, const request_t *req) {
  Sieve_size_params_t *params = cache->eviction_params;

  /* if we have run one full around or first eviction */
  cache_obj_t *obj = params->pointer == NULL ? params->q_tail : params->pointer;

  while (obj->sieve_size.freq > 0) {
    obj->sieve_size.freq -= 1;
    obj = obj->queue.prev == NULL ? params->q_tail : obj->queue.prev;
  }

  cache_obj_t *obj_to_evict = Sieve_size_evict_largest_from_window(cache, obj, 0);

  if (obj_to_evict == obj) {
    params->pointer = obj->queue.prev;
  } else {
    params->pointer = obj;
  }
  
  remove_obj_from_list(&params->q_head, &params->q_tail, obj_to_evict);
  cache_evict_base(cache, obj_to_evict, true);
}

static void Sieve_size_remove_obj(cache_t *cache, cache_obj_t *obj_to_remove) {
  DEBUG_ASSERT(obj_to_remove != NULL);
  Sieve_size_params_t *params = cache->eviction_params;
  if (obj_to_remove == params->pointer) {
    params->pointer = obj_to_remove->queue.prev;
  }
  remove_obj_from_list(&params->q_head, &params->q_tail, obj_to_remove);
  cache_remove_obj_base(cache, obj_to_remove, true);
}

/**
 * @brief remove an object from the cache
 * this is different from cache_evict because it is used to for user trigger
 * remove, and eviction is used by the cache to make space for new objects
 *
 * it needs to call cache_remove_obj_base before returning
 * which updates some metadata such as n_obj, occupied size, and hash table
 *
 * @param cache
 * @param obj_id
 * @return true if the object is removed, false if the object is not in the
 * cache
 */
static bool Sieve_size_remove(cache_t *cache, const obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }

  Sieve_size_remove_obj(cache, obj);

  return true;
}

static void Sieve_size_verify(cache_t *cache) {
  Sieve_size_params_t *params = cache->eviction_params;
  int64_t n_obj = 0, n_byte = 0;
  cache_obj_t *obj = params->q_head;

  while (obj != NULL) {
    assert(hashtable_find_obj_id(cache->hashtable, obj->obj_id) != NULL);
    n_obj++;
    n_byte += obj->obj_size;
    obj = obj->queue.next;
  }

  assert(n_obj == cache->get_n_obj(cache));
  assert(n_byte == cache->get_occupied_byte(cache));
}

#ifdef __cplusplus
}
#endif
