/*
 *
 * Copyright (C) 2023 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 */

#ifndef UMF_JEMALLOC_MEMORY_POOL_H
#define UMF_JEMALLOC_MEMORY_POOL_H 1

#ifdef __cplusplus
#include <atomic>
extern "C" {
using namespace std;
#else
#include <stdatomic.h>
#endif

#include <limits.h>
#include <assert.h>
#include <stdbool.h>
#include <umf/memory_pool.h>
#include <umf/memory_pool_ops.h>
#include <jemalloc/jemalloc.h>
//#include <../src/memory_pool_internal.h>

#include <umf/base.h>
#include <umf/memory_pool.h>
#include <umf/memory_pool_ops.h>
#include <umf/memory_provider.h>

#include <stdbool.h>

typedef struct umf_memory_pool_t {
    void *pool_priv;
    umf_memory_pool_ops_t ops;
    umf_pool_create_flags_t flags;

    // Memory provider used by the pool.
    umf_memory_provider_handle_t provider;
} umf_memory_pool_t;


#define MAX_JEMALLOC_THREADS 200

/// @brief Configuration of Jemalloc Pool
typedef struct umf_jemalloc_pool_params_t {
    /// Set to true if umfMemoryProviderFree() should never be called.
    bool disable_provider_free;
} umf_jemalloc_pool_params_t;

umf_memory_pool_ops_t *umfJemallocPoolOps(void);


extern __thread unsigned arena_spin;
extern __thread unsigned thread_id;
extern atomic_int thread_count;
inline unsigned __attribute__((always_inline)) tid(){
	if(thread_id==UINT_MAX){
		thread_id = atomic_fetch_add_explicit(&thread_count, 1, memory_order_relaxed);
		arena_spin = thread_id;
	}
	return thread_id;
}

typedef struct jemalloc_memory_pool_t {
    umf_memory_provider_handle_t provider;
    unsigned arena_index; // base index of jemalloc arena
	unsigned num_arenas; // range of associated indices
	unsigned tcaches[MAX_JEMALLOC_THREADS];
    // set to true if umfMemoryProviderFree() should never be called
    bool disable_provider_free;
} jemalloc_memory_pool_t;


inline void* __attribute__((always_inline))
umfFastJemallocMalloc(umf_memory_pool_handle_t hPool, size_t size){
	assert(hPool!=NULL);

    jemalloc_memory_pool_t *je_pool = (jemalloc_memory_pool_t *)((void*)hPool->pool_priv);
	assert(je_pool);
	
	/*
	cycle through arenas associated with our pool
	use tcache associated with this ppol
	*/
	arena_spin++;
	if(arena_spin>=je_pool->num_arenas){arena_spin=0;}
	int arena = je_pool->arena_index + arena_spin;
    int flags = MALLOCX_ARENA(arena) | MALLOCX_TCACHE(je_pool->tcaches[tid()]);
    void *ptr = mallocx(size, flags);
    if (ptr == NULL) {
        //TLS_last_allocation_error = UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    //VALGRIND_DO_MEMPOOL_ALLOC(hPool, ptr, size);

    return ptr;	
}

inline  __attribute__((always_inline))
umf_result_t umfFastJemallocFree(umf_memory_pool_handle_t hPool, void* ptr){ 
    jemalloc_memory_pool_t *je_pool = (jemalloc_memory_pool_t *)((void*)hPool->pool_priv);

    assert(je_pool);

    if (ptr != NULL) {
        //VALGRIND_DO_MEMPOOL_FREE(hPool, ptr);
        dallocx(ptr, MALLOCX_TCACHE(je_pool->tcaches[tid()]));
    }

    return UMF_RESULT_SUCCESS;
}



#ifdef __cplusplus
}
#endif

#endif /* UMF_JEMALLOC_MEMORY_POOL_H */
