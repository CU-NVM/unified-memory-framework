/*
 * Copyright (C) 2022-2024 Intel Corporation
 *
 * Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base_alloc_global.h"
#include "utils_common.h"
#include "utils_concurrency.h"
#include "utils_log.h"
#include "utils_sanitizers.h"

#include <umf/memory_pool.h>
#include <umf/memory_pool_ops.h>
#include <umf/pools/pool_jemalloc.h>

#include <jemalloc/jemalloc.h>

#include <threads.h>
#include <stdatomic.h>


// The Windows version of jemalloc uses API with je_ prefix,
// while the Linux one does not.
#ifndef _WIN32
#define je_mallocx mallocx
#define je_dallocx dallocx
#define je_rallocx rallocx
// #define je_mallctl mallctl
#define je_malloc_usable_size malloc_usable_size
#endif

__thread unsigned thread_id=UINT_MAX;
__thread unsigned arena_spin=0;
atomic_int thread_count=0;

#define MALLOCX_ARENA_MAX (MALLCTL_ARENAS_ALL - 1)

// unsigned set_tcache(jemalloc_memory_pool_t* je_pool, unsigned tid, unsigned tcache){
//     assert(je_pool);
//     // assert(je_pool->tcaches_resize_lk);
//     access_tcaches:
//     pthread_rwlock_rdlock(&je_pool->tcaches_resize_lk);
//     if(tid < je_pool->tcaches_size){
//         int size = je_pool->tcaches_size;
//         je_pool->tcaches[tid] = tcache;
//         pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//     } else {
//         pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//         pthread_rwlock_wrlock(&je_pool->tcaches_resize_lk);
//         if (tid>= je_pool->tcaches_size){
//             //resizing tcaches
//             unsigned *temp_tcaches = (unsigned *)realloc(je_pool->tcaches, (tid+1) * sizeof(unsigned));
//             if (!temp_tcaches) {
//                 // printf("temp_tcaches realloc failed");
//                 pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//                 exit(EXIT_FAILURE);
//             }
//             je_pool->tcaches = temp_tcaches;
//             je_pool->tcaches_size = tid+1;
//         }
//         pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//         goto access_tcaches;
//     }
// }

// unsigned get_tcache(jemalloc_memory_pool_t* je_pool, unsigned tid ){
//     assert(je_pool);
//     // assert(je_pool->tcaches_resize_lk);
//     access_tcaches:
//     pthread_rwlock_rdlock(&je_pool->tcaches_resize_lk);
//     if(tid < je_pool->tcaches_size){
//         unsigned tcache_tid = je_pool->tcaches[tid];
//         // pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//         return tcache_tid;
//     } else {
//         pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//         pthread_rwlock_wrlock(&je_pool->tcaches_resize_lk);
//         if (tid>= je_pool->tcaches_size){
//             //resising tcaches
//             unsigned *temp_tcaches = (unsigned *)realloc(je_pool->tcaches, (tid+1) * sizeof(unsigned));
//             if (!temp_tcaches) {
//                 // printf("temp_tcaches realloc failed");
//                 pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//                 exit(EXIT_FAILURE);
//             }
//             je_pool->tcaches = temp_tcaches;
//             je_pool->tcaches_size = tid+1;
//         }
//         pthread_rwlock_unlock(&je_pool->tcaches_resize_lk);
//         goto access_tcaches;
//     }
// }












static __TLS umf_result_t TLS_last_allocation_error;

static jemalloc_memory_pool_t *pool_by_arena_index[MALLCTL_ARENAS_ALL];

static jemalloc_memory_pool_t *get_pool_by_arena_index(unsigned arena_ind) {
    // there is no way to obtain MALLOCX_ARENA_MAX from jemalloc
    // so this checks if arena_ind does not exceed assumed range
    assert(arena_ind < MALLOCX_ARENA_MAX);
    return pool_by_arena_index[arena_ind];
}

// arena_extent_alloc - an extent allocation function conforms to the extent_alloc_t type and upon
// success returns a pointer to size bytes of mapped memory on behalf of arena arena_ind such that
// the extent's base address is a multiple of alignment, as well as setting *zero to indicate
// whether the extent is zeroed and *commit to indicate whether the extent is committed. Upon error
// the function returns NULL and leaves *zero and *commit unmodified. The size parameter is always
// a multiple of the page size. The alignment parameter is always a power of two at least as large
// as the page size. Zeroing is mandatory if *zero is true upon function entry. Committing is
// mandatory if *commit is true upon function entry. If new_addr is not NULL, the returned pointer
// must be new_addr on success or NULL on error. Committed memory may be committed in absolute
// terms as on a system that does not overcommit, or in implicit terms as on a system that
// overcommits and satisfies physical memory needs on demand via soft page faults. Note that
// replacing the default extent allocation function makes the arena's arena.<i>.dss setting irrelevant.
// (from https://jemalloc.net/jemalloc.3.html)
static void *arena_extent_alloc(extent_hooks_t *extent_hooks, void *new_addr,
                                size_t size, size_t alignment, bool *zero,
                                bool *commit, unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)arena_ind;    // unused
    umf_result_t ret;

    jemalloc_memory_pool_t *pool = get_pool_by_arena_index(arena_ind);

    void *ptr = new_addr;
    ret = umfMemoryProviderAlloc(pool->provider, size, alignment, &ptr);
    if (ret != UMF_RESULT_SUCCESS) {
        return NULL;
    }

    if (new_addr != NULL && ptr != new_addr) {
        if (!pool->disable_provider_free) {
            umfMemoryProviderFree(pool->provider, ptr, size);
        }
        return NULL;
    }

#ifndef __SANITIZE_ADDRESS__
    // jemalloc might write to new extents in realloc, so we cannot
    // mark them as unaccessible under asan
    utils_annotate_memory_inaccessible(ptr, size);
#endif

    if (*zero) {
        utils_annotate_memory_defined(ptr, size);
        memset(ptr, 0, size); // TODO: device memory is not accessible by host
    }

    *commit = true;

    return ptr;
}

// arena_extent_destroy - an extent destruction function conforms to the extent_destroy_t type and
// unconditionally destroys an extent at given addr and size with committed/decommited memory as
// indicated, on behalf of arena arena_ind. This function may be called to destroy retained extents
// during arena destruction (see arena.<i>.destroy).
// (from https://jemalloc.net/jemalloc.3.html)
static void arena_extent_destroy(extent_hooks_t *extent_hooks, void *addr,
                                 size_t size, bool committed,
                                 unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)committed;    // unused
    (void)arena_ind;    // unused

    jemalloc_memory_pool_t *pool = get_pool_by_arena_index(arena_ind);

    if (pool->disable_provider_free) {
        return;
    }

    umf_result_t ret;
    ret = umfMemoryProviderFree(pool->provider, addr, size);
    if (ret != UMF_RESULT_SUCCESS) {
        LOG_ERR("umfMemoryProviderFree failed");
    }
}

// arena_extent_dalloc - an extent deallocation function conforms to the extent_dalloc_t type and
// deallocates an extent at given addr and size with committed/decommited memory as indicated, on
// behalf of arena arena_ind, returning false upon success. If the function returns true,
// this indicates opt-out from deallocation; the virtual memory mapping associated with the extent
// remains mapped, in the same commit state, and available for future use, in which case it will be
// automatically retained for later reuse.
// (from https://jemalloc.net/jemalloc.3.html)
static bool arena_extent_dalloc(extent_hooks_t *extent_hooks, void *addr,
                                size_t size, bool committed,
                                unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)committed;    // unused

    jemalloc_memory_pool_t *pool = get_pool_by_arena_index(arena_ind);

    if (pool->disable_provider_free) {
        return true; // opt-out from deallocation
    }

    umf_result_t ret;
    ret = umfMemoryProviderFree(pool->provider, addr, size);
    if (ret != UMF_RESULT_SUCCESS) {
        LOG_ERR("umfMemoryProviderFree failed in dalloc");
    }

    return ret != UMF_RESULT_SUCCESS;
}

// arena_extent_commit - an extent commit function conforms to the extent_commit_t type and commits
// zeroed physical memory to back pages within an extent at given addr and size at offset bytes,
// extending for length on behalf of arena arena_ind, returning false upon success. Committed memory
// may be committed in absolute terms as on a system that does not overcommit, or in implicit terms
// as on a system that overcommits and satisfies physical memory needs on demand via soft page faults.
// If the function returns true, this indicates insufficient physical memory to satisfy the request.
// (from https://jemalloc.net/jemalloc.3.html)
static bool arena_extent_commit(extent_hooks_t *extent_hooks, void *addr,
                                size_t size, size_t offset, size_t length,
                                unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)addr;         // unused
    (void)size;         // unused
    (void)offset;       // unused
    (void)length;       // unused
    (void)arena_ind;    // unused

    // TODO: add this function to the provider API to support Windows and USM
    return false; // false means success (commit is a nop)
}

// arena_extent_decommit - an extent decommit function conforms to the extent_decommit_t type
// and decommits any physical memory that is backing pages within an extent at given addr and size
// at offset bytes, extending for length on behalf of arena arena_ind, returning false upon success,
// in which case the pages will be committed via the extent commit function before being reused.
// If the function returns true, this indicates opt-out from decommit; the memory remains committed
// and available for future use, in which case it will be automatically retained for later reuse.
// (from https://jemalloc.net/jemalloc.3.html)
static bool arena_extent_decommit(extent_hooks_t *extent_hooks, void *addr,
                                  size_t size, size_t offset, size_t length,
                                  unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)addr;         // unused
    (void)size;         // unused
    (void)offset;       // unused
    (void)length;       // unused
    (void)arena_ind;    // unused

    // TODO: add this function to the provider API to support Windows and USM
    return true; // true means failure (unsupported)
}

// arena_extent_purge_lazy - an extent purge function conforms to the extent_purge_t type and discards
// physical pages within the virtual memory mapping associated with an extent at given addr and size
// at offset bytes, extending for length on behalf of arena arena_ind. A lazy extent purge function
// (e.g. implemented via madvise(...MADV_FREE)) can delay purging indefinitely and leave the pages
// within the purged virtual memory range in an indeterminite state, whereas a forced extent purge
// function immediately purges, and the pages within the virtual memory range will be zero-filled
// the next time they are accessed. If the function returns true, this indicates failure to purge.
// (from https://jemalloc.net/jemalloc.3.html)
static bool arena_extent_purge_lazy(extent_hooks_t *extent_hooks, void *addr,
                                    size_t size, size_t offset, size_t length,
                                    unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)size;         // unused
    (void)arena_ind;    // unused

    jemalloc_memory_pool_t *pool = get_pool_by_arena_index(arena_ind);

    umf_result_t ret = umfMemoryProviderPurgeLazy(
        pool->provider, (char *)addr + offset, length);

    return (ret != UMF_RESULT_SUCCESS); // false means success
}

// arena_extent_purge_forced - see arena_extent_purge_lazy above
static bool arena_extent_purge_forced(extent_hooks_t *extent_hooks, void *addr,
                                      size_t size, size_t offset, size_t length,
                                      unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)size;         // unused
    (void)arena_ind;    // unused

    jemalloc_memory_pool_t *pool = get_pool_by_arena_index(arena_ind);

    umf_result_t ret = umfMemoryProviderPurgeForce(
        pool->provider, (char *)addr + offset, length);

    return (ret != UMF_RESULT_SUCCESS); // false means success
}

// arena_extent_split - an extent split function conforms to the extent_split_t type and optionally
// splits an extent at given addr and size into two adjacent extents, the first of size_a bytes,
// and the second of size_b bytes, operating on committed/decommitted memory as indicated,
// on behalf of arena arena_ind, returning false upon success. If the function returns true,
// this indicates that the extent remains unsplit and therefore should continue to be operated on as a whole.
// (from https://jemalloc.net/jemalloc.3.html)
static bool arena_extent_split(extent_hooks_t *extent_hooks, void *addr,
                               size_t size, size_t size_a, size_t size_b,
                               bool committed, unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)committed;    // unused
    (void)size_b;

    assert(size_a + size_b == size);

    jemalloc_memory_pool_t *pool = get_pool_by_arena_index(arena_ind);
    assert(pool);
    return umfMemoryProviderAllocationSplit(pool->provider, addr, size,
                                            size_a) != UMF_RESULT_SUCCESS;
}

// arena_extent_merge - an extent merge function conforms to the extent_merge_t type and optionally
// merges adjacent extents, at given addr_a and size_a with given addr_b and size_b into one
// contiguous extent, operating on committed/decommitted memory as indicated, on behalf of arena arena_ind,
// returning false upon success. If the function returns true, this indicates that the extents
// remain distinct mappings and therefore should continue to be operated on independently.
// (from https://jemalloc.net/jemalloc.3.html)
static bool arena_extent_merge(extent_hooks_t *extent_hooks, void *addr_a,
                               size_t size_a, void *addr_b, size_t size_b,
                               bool committed, unsigned arena_ind) {
    (void)extent_hooks; // unused
    (void)committed;    // unused

    jemalloc_memory_pool_t *pool = get_pool_by_arena_index(arena_ind);
    assert(pool);
    return umfMemoryProviderAllocationMerge(pool->provider, addr_a, addr_b,
                                            size_a + size_b) !=
           UMF_RESULT_SUCCESS;
}

// The extent_hooks_t structure comprises function pointers which are described individually below.
// jemalloc uses these functions to manage extent lifetime, which starts off with allocation
// of mapped committed memory, in the simplest case followed by deallocation. However, there are
// performance and platform reasons to retain extents for later reuse. Cleanup attempts cascade
// from deallocation to decommit to forced purging to lazy purging, which gives the extent
// management functions opportunities to reject the most permanent cleanup operations in favor
// of less permanent (and often less costly) operations. All operations except allocation can be
// universally opted out of by setting the hook pointers to NULL, or selectively opted out of
// by returning failure. Note that once the extent hook is set, the structure is accessed directly
// by the associated arenas, so it must remain valid for the entire lifetime of the arenas.
// (from https://jemalloc.net/jemalloc.3.html)
static extent_hooks_t arena_extent_hooks = {
    .alloc = arena_extent_alloc,
    .dalloc = arena_extent_dalloc,
    .destroy = arena_extent_destroy,
    .commit = arena_extent_commit,
    .decommit = arena_extent_decommit,
    .purge_lazy = arena_extent_purge_lazy,
    .purge_forced = arena_extent_purge_forced,
    .split = arena_extent_split,
    .merge = arena_extent_merge,
};

static void *op_malloc(void *pool, size_t size) {
    assert(pool);
    jemalloc_memory_pool_t *je_pool = (jemalloc_memory_pool_t *)pool;
    // MALLOCX_TCACHE_NONE is set, because jemalloc can mix objects from different arenas inside
    // the tcache, so we wouldn't be able to guarantee isolation of different providers.
	
	/*
	cycle through arenas associated with our pool
	use tcache associated with this ppol
	*/
	arena_spin++;
	if(arena_spin>=je_pool->num_arenas){arena_spin=0;}
	int arena = je_pool->arena_index + arena_spin;
    // int flags = MALLOCX_ARENA(arena) | MALLOCX_TCACHE(je_pool->tcaches[tid()]);
    int flags = MALLOCX_ARENA(arena) | MALLOCX_TCACHE(get_tcache(je_pool,tid()));
    void *ptr = je_mallocx(size, flags);
    if (ptr == NULL) {
        TLS_last_allocation_error = UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    VALGRIND_DO_MEMPOOL_ALLOC(pool, ptr, size);

    return ptr;
}

static umf_result_t op_free(void *pool, void *ptr) {
    (void)pool; // unused
    assert(pool);
    jemalloc_memory_pool_t *je_pool = (jemalloc_memory_pool_t *)pool;


    if (ptr != NULL) {
        VALGRIND_DO_MEMPOOL_FREE(pool, ptr);
        // je_dallocx(ptr, MALLOCX_TCACHE(je_pool->tcaches[tid()]));
        je_dallocx(ptr, MALLOCX_TCACHE(get_tcache(je_pool,tid())));
        
    }

    return UMF_RESULT_SUCCESS;
}

static void *op_calloc(void *pool, size_t num, size_t size) {
    assert(pool);
    size_t csize = num * size;
    void *ptr = op_malloc(pool, csize);
    if (ptr == NULL) {
        // TLS_last_allocation_error is set by op_malloc()
        return NULL;
    }

    utils_annotate_memory_defined(ptr, num * size);

    memset(ptr, 0, csize); // TODO: device memory is not accessible by host
    return ptr;
}

static void *op_realloc(void *pool, void *ptr, size_t size) {
    assert(pool);
    jemalloc_memory_pool_t *je_pool = (jemalloc_memory_pool_t *)pool;

    if (size == 0 && ptr != NULL) {
        // je_dallocx(ptr, MALLOCX_TCACHE(je_pool->tcaches[tid()]));
        je_dallocx(ptr, MALLOCX_TCACHE(get_tcache(je_pool,tid())));
        TLS_last_allocation_error = UMF_RESULT_SUCCESS;
        VALGRIND_DO_MEMPOOL_FREE(pool, ptr);
        return NULL;
    } else if (ptr == NULL) {
        return op_malloc(pool, size);
    }
    // MALLOCX_TCACHE_NONE is set, because jemalloc can mix objects from different arenas inside
    // the tcache, so we wouldn't be able to guarantee isolation of different providers.
	arena_spin++;
	if(arena_spin>=je_pool->num_arenas){arena_spin=0;}
	int arena = je_pool->arena_index + arena_spin;
    // int flags = MALLOCX_ARENA(arena) | MALLOCX_TCACHE(je_pool->tcaches[tid()]);
    int flags = MALLOCX_ARENA(arena) | MALLOCX_TCACHE(get_tcache(je_pool,tid()));
    void *new_ptr = je_rallocx(ptr, size, flags);
    if (new_ptr == NULL) {
        TLS_last_allocation_error = UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    if (new_ptr != ptr) {
        VALGRIND_DO_MEMPOOL_ALLOC(pool, new_ptr, size);
        VALGRIND_DO_MEMPOOL_FREE(pool, ptr);

        // memory was copied from old ptr so it's now defined
        utils_annotate_memory_defined(new_ptr, size);
    }

    return new_ptr;
}

static void *op_aligned_alloc(void *pool, size_t size, size_t alignment) {
    assert(pool);
    jemalloc_memory_pool_t *je_pool = (jemalloc_memory_pool_t *)pool;
	arena_spin++;
	if(arena_spin>=je_pool->num_arenas){arena_spin=0;}
	int arena = je_pool->arena_index + arena_spin;
    // int flags = MALLOCX_ALIGN(alignment) | MALLOCX_ARENA(arena) | MALLOCX_TCACHE(je_pool->tcaches[tid()]);
    int flags = MALLOCX_ALIGN(alignment) | MALLOCX_ARENA(arena) | MALLOCX_TCACHE(get_tcache(je_pool,tid()));
    // MALLOCX_TCACHE_NONE is set, because jemalloc can mix objects from different arenas inside
    // the tcache, so we wouldn't be able to guarantee isolation of different providers.
    void *ptr = je_mallocx(size, flags);
    if (ptr == NULL) {
        TLS_last_allocation_error = UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
        return NULL;
    }

    VALGRIND_DO_MEMPOOL_ALLOC(pool, ptr, size);

    return ptr;
}

static umf_result_t op_initialize(umf_memory_provider_handle_t provider,
                                  void *params, void **out_pool) {
    assert(provider);
    assert(out_pool);

    umf_jemalloc_pool_params_t *je_params =
        (umf_jemalloc_pool_params_t *)params;

    extent_hooks_t *pHooks = &arena_extent_hooks;
    size_t unsigned_size = sizeof(unsigned);
    int err;

    jemalloc_memory_pool_t *pool =
        umf_ba_global_alloc(sizeof(jemalloc_memory_pool_t));
    if (!pool) {
        return UMF_RESULT_ERROR_OUT_OF_HOST_MEMORY;
    }

    pool->provider = provider;
	pool->num_arenas = 160;
    pool->tcaches_size = 1;
    printf("Tcaches_size is %zu\n",pool->tcaches_size);
    pool->tcaches = malloc(pool->tcaches_size * sizeof(unsigned));
    int lk_init_fail = pthread_rwlock_init(&pool->tcaches_resize_lk, NULL);
    assert(lk_init_fail==0);
    if (je_params) {
        pool->disable_provider_free = je_params->disable_provider_free;
    } else {
        pool->disable_provider_free = false;
    }

	unsigned new_arena_index;
	for(unsigned i = 0; i<pool->num_arenas; i++){
		err = je_mallctl("arenas.create", (void *)&new_arena_index, &unsigned_size,
						 NULL, 0);
		if (err) {
			LOG_ERR("Could not create arena.");
			goto err_free_pool;
		}

		// setup extent_hooks for newly created arena
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "arena.%u.extent_hooks", new_arena_index);
		err = je_mallctl(cmd, NULL, NULL, (void *)&pHooks, sizeof(void *));
		if (err) {
			snprintf(cmd, sizeof(cmd), "arena.%u.destroy", new_arena_index);
			je_mallctl(cmd, NULL, 0, NULL, 0);
			LOG_ERR("Could not setup extent_hooks for newly created arena.");
			goto err_free_pool;
		}

		if(i == 0){
			pool->arena_index = new_arena_index; // set the base index
		}
		pool_by_arena_index[new_arena_index] = pool;

		*out_pool = (umf_memory_pool_handle_t)pool;

		VALGRIND_DO_CREATE_MEMPOOL(pool, 0, 0);
	}
	
	// assert(MAX_JEMALLOC_THREADS<250);
    assert(pool);
    printf("Initial size:: %zu\n",pool->tcaches_size);
    // changed i < MAX_JEMALLOC_THREADS to i < tcaches_size
    for(unsigned i = 0; i<pool->tcaches_size;i++){
	// for(unsigned i = 0; i<MAX_JEMALLOC_THREADS;i++){
		unsigned tcache;
		size_t sz = sizeof(unsigned);
		je_mallctl("tcache.create",&tcache,&sz,NULL,0);
		// pool->tcaches[i] = tcache;
        set_tcache(pool,i,tcache);
		// printf("Creating tcache: %d\n",tcache);
	}
	
    return UMF_RESULT_SUCCESS;

err_free_pool:
	assert(false && "Failed to create a pool, error checking not implemented");
	exit(-1);
    umf_ba_global_free(pool);
    return UMF_RESULT_ERROR_MEMORY_PROVIDER_SPECIFIC;
}

static void op_finalize(void *pool) {
    assert(pool);
    jemalloc_memory_pool_t *je_pool = (jemalloc_memory_pool_t *)pool;
    char cmd[64];
	for(unsigned i = 0; i<je_pool->num_arenas; i++){
		snprintf(cmd, sizeof(cmd), "arena.%u.destroy", je_pool->arena_index);
		je_mallctl(cmd, NULL, 0, NULL, 0);
		pool_by_arena_index[je_pool->arena_index] = NULL;		
	}
    // replace MAX_JEMALLOC_THREADS with pool->tcaches_size
	// for(unsigned i = 0; i< MAX_JEMALLOC_THREADS;i++){
    for(unsigned i = 0; i< je_pool->tcaches_size;i++){
		// unsigned tcache = je_pool->tcaches[i];
        unsigned tcache = get_tcache(je_pool,i);
		size_t sz = sizeof(unsigned);
		je_mallctl("tcache.destroy",NULL,0,&tcache,sz);
	}
	free(je_pool->tcaches); 
    umf_ba_global_free(je_pool);

    VALGRIND_DO_DESTROY_MEMPOOL(pool);
}

static size_t op_malloc_usable_size(void *pool, void *ptr) {
    (void)pool; // not used
    return je_malloc_usable_size(ptr);
}

static umf_result_t op_get_last_allocation_error(void *pool) {
    (void)pool; // not used
    return TLS_last_allocation_error;
}

static umf_memory_pool_ops_t UMF_JEMALLOC_POOL_OPS = {
    .version = UMF_VERSION_CURRENT,
    .initialize = op_initialize,
    .finalize = op_finalize,
    .malloc = op_malloc,
    .calloc = op_calloc,
    .realloc = op_realloc,
    .aligned_malloc = op_aligned_alloc,
    .malloc_usable_size = op_malloc_usable_size,
    .free = op_free,
    .get_last_allocation_error = op_get_last_allocation_error,
};

umf_memory_pool_ops_t *umfJemallocPoolOps(void) {
    return &UMF_JEMALLOC_POOL_OPS;
}
