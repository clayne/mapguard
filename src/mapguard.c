/* Reference implementation of map guard
 * Copyright Chris Rohlf - 2019 */

#include "mapguard.h"

__attribute__((constructor)) void mapguard_ctor() {
    /* Enable configuration of mapguard via environment
     * variables during DSO load time only */
    ENV_TO_INT(MG_DISALLOW_RWX, g_mapguard_policy.disallow_rwx);
    ENV_TO_INT(MG_DISALLOW_TRANSITION_TO_X, g_mapguard_policy.disallow_transition_to_x);
    ENV_TO_INT(MG_DISALLOW_TRANSITION_FROM_X, g_mapguard_policy.disallow_transition_from_x);
    ENV_TO_INT(MG_DISALLOW_STATIC_ADDRESS, g_mapguard_policy.disallow_static_address);
    ENV_TO_INT(MG_ENABLE_GUARD_PAGES, g_mapguard_policy.enable_guard_pages);
    ENV_TO_INT(MG_PANIC_ON_VIOLATION, g_mapguard_policy.panic_on_violation);
    ENV_TO_INT(MG_POISON_ON_ALLOCATION, g_mapguard_policy.poison_on_allocation);
    ENV_TO_INT(MG_USE_MAPPING_CACHE, g_mapguard_policy.use_mapping_cache);

    g_real_mmap = dlsym(RTLD_NEXT, "mmap");
    g_real_munmap = dlsym(RTLD_NEXT, "munmap");
    g_real_mprotect = dlsym(RTLD_NEXT, "mprotect");
    g_real_mremap = dlsym(RTLD_NEXT, "mremap");

    vector_init(&g_map_cache_vector);

    g_page_size = getpagesize();
}

__attribute((destructor)) void mapguard_dtor() {
    /* Erase all cache entries */
    if(g_mapguard_policy.use_mapping_cache) {
        vector_delete_callback_t *dc = &vector_pointer_free;
        vector_delete_all(&g_map_cache_vector, dc);
        vector_free(&g_map_cache_vector);
    }
}

mapguard_cache_entry_t *new_mapguard_cache_entry() {
    mapguard_cache_entry_t *mce = (mapguard_cache_entry_t *) malloc(sizeof(mapguard_cache_entry_t));
    memset(mce, 0x0, sizeof(mapguard_cache_entry_t));
    return mce;
}

void vector_pointer_free(void *p) {
    free(p);
}

int32_t env_to_int(char *string) {
    char *p = getenv(string);

    if(p == NULL) {
        return 0;
    }

    return strtoul(p, NULL, 0);
}

void *get_base_page(void *addr) {
    return (void *) ((uintptr_t) addr & ~(g_page_size-1));
}

/* Checks if we have a cache entry for this mapping */
void *is_mapguard_entry_cached(void *p, void *data) {
    mapguard_cache_entry_t *mce = (mapguard_cache_entry_t *) p;

    if(mce->start == data) {
        return mce;
    }

    /* This address is within the range of a cached mapping */
    if(data > mce->start && mce->start+mce->size > data) {
        return mce;
    }

    return NULL;
}

void *map_guard_page(void *addr) {
    return g_real_mmap(get_base_page(addr), g_page_size, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
}

void unmap_top_guard_page(mapguard_cache_entry_t *mce) {
    if(mce->guard_top) {
        g_real_munmap(mce->guard_top, g_page_size);
        LOG("Unmapped top guard page %p", mce->guard_top);
    }
}

void unmap_bottom_guard_page(mapguard_cache_entry_t *mce) {
    if(mce->guard_bottom) {
        g_real_munmap(mce->guard_bottom, g_page_size);
        LOG("Unmapped bottom guard page %p", mce->guard_bottom);
    }
}

void unmap_guard_pages(mapguard_cache_entry_t *mce) {
    unmap_bottom_guard_page(mce);
    unmap_top_guard_page(mce);
}

void map_bottom_guard_page(mapguard_cache_entry_t *mce) {
    if(mce == NULL) {
        return;
    }

    mce->guard_bottom = map_guard_page(get_base_page(mce->start-1));

#ifdef DEBUG
    if(mce->guard_bottom != MAP_FAILED) {
        LOG("Successfully allocated bottom guard page: [(guard) %p | %p]", mce->guard_bottom, mce->start);
    } else {
        LOG("Failed to map bottom guard page @ %p. Mapped @ %p", get_base_page(mce->start), mce->guard_bottom);
    }
#endif
}

void map_top_guard_page(mapguard_cache_entry_t *mce) {
    if(mce == NULL) {
        return;
    }

    mce->guard_top = map_guard_page((void *) ROUND_UP_PAGE((uint64_t)(mce->start+mce->size)));

#ifdef DEBUG
    if(mce->guard_top != MAP_FAILED && mce->guard_top > (mce->start+mce->size)) {
        LOG("Successfully allocated top guard page: [%p | %p (guard)]", mce->start, mce->guard_top);
    } else {
        LOG("Failed to map top guard page @ %p. Mapped @ %p", (void *) ROUND_UP_PAGE((uint64_t)(mce->start+mce->size)), mce->guard_top);
    }
#endif
}

void map_guard_pages(mapguard_cache_entry_t *mce) {
    if(mce->start == 0 && mce->size != 0) {
        return;
    }

    map_bottom_guard_page(mce);
    map_top_guard_page(mce);
}

/* Hook mmap in libc */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    /* We don't intercept file backed mappings */
    if(fd != -1) {
        void *map_ptr = g_real_mmap(addr, length, prot, flags, fd, offset);
        return map_ptr;
    }

    /* Evaluate and enforce security policies set by env vars */

    /* Disallow RWX mappings */
    if(g_mapguard_policy.disallow_rwx && (prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        LOG("Disallowing RWX memory allocation");
        MAYBE_PANIC
        return MAP_FAILED;
    }

    /* Disallow mappings at a hardcoded address. This weakens ASLR */
    if(addr != 0 && g_mapguard_policy.disallow_static_address) {
        LOG("Disallowing memory allocation at static address %p", addr);
        MAYBE_PANIC
        return MAP_FAILED;
    }

    void *map_ptr = g_real_mmap(addr, length, prot, flags, fd, offset);

    if(map_ptr == MAP_FAILED) {
        return map_ptr;
    }

    mapguard_cache_entry_t *mce = NULL;

    /* Cache the start, size and protections of this mapping */
    if(g_mapguard_policy.use_mapping_cache) {
        mce = new_mapguard_cache_entry();
        mce->start = map_ptr;
        mce->size = length;
        mce->immutable_prot |= prot;
        mce->current_prot = prot;
        mce->cache_index = vector_push(&g_map_cache_vector, mce);
    }

    /* Allocate guard pages. This is a 'best effort' attempt because
     * we don't know if existing mappings are below/above the page(s)
     * the caller just allocated. Calculating where space is available
     * for 2 guard pages and the user requested allocation creates too
     * much of a performance impact */
    if(g_mapguard_policy.enable_guard_pages && mce) {
        map_guard_pages(mce);
    }

    /* Set all bytes in the allocation if configured and pages are writeable */
    if(g_mapguard_policy.poison_on_allocation && (prot & PROT_WRITE)) {
        memset(map_ptr, MG_POISON_BYTE, length);
    }

    return map_ptr;
}

/* Hook munmap in libc */
int munmap(void *addr, size_t length) {
    mapguard_cache_entry_t *mce = NULL;
    int32_t ret = 0;

    /* Remove tracked pages from the cache */
    if(g_mapguard_policy.use_mapping_cache) {
        mce = (mapguard_cache_entry_t *) vector_for_each(&g_map_cache_vector, (vector_for_each_callback_t *) is_mapguard_entry_cached, (void *) addr);

        if(mce) {
            LOG("Found mapguard cache entry for mapping %p", mce->start);

            /* Handle a partial unmapping */
            if(mce->start != addr || mce->size != length) {
                /* Update the size we are tracking */
                mce->size -= length;

                /* Handle the case of unmapping the last N pages */
                if(addr > mce->start && length < mce->size) {
                    unmap_top_guard_page(mce);
                    ret = g_real_munmap(addr, length);

                    /* If the unmapping succeeded remap the top guard page */
                    if(ret == 0) {
                        map_top_guard_page(mce);
                    }

                    return ret;
                }

                /* Handle the case of unmapping the first N pages */
                if(mce->start == addr) {
                    unmap_bottom_guard_page(mce);
                    mce->start += length;
                    ret = g_real_munmap(addr, length);

                    /* If the unmapping succeeded remap the bottom guard page */
                    if(ret == 0) {
                        map_bottom_guard_page(mce);
                    }

                    return ret;
                }
            } else {
                unmap_guard_pages(mce);
                LOG("Deleting cache entry for %p", mce->start);
                vector_delete_at(&g_map_cache_vector, mce->cache_index);
                free(mce);
                mce = NULL;
            }
        }
    }

    return g_real_munmap(addr, length);;
}

/* Hook mprotect in libc */
int mprotect(void *addr, size_t len, int prot) {
    mapguard_cache_entry_t *mce = NULL;

    /* Disallow RWX mappings */
    if(g_mapguard_policy.disallow_rwx && (prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        LOG("Disallowing RWX mprotect");
        MAYBE_PANIC
        return ERROR;
    }

    /* Disallow transition to/from X (requires the mapping cache) */
    if(g_mapguard_policy.use_mapping_cache) {
        mce = (mapguard_cache_entry_t *) vector_for_each(&g_map_cache_vector, (vector_for_each_callback_t *) is_mapguard_entry_cached, (void *) addr);

        if(mce != NULL && mce->xom_enabled == 0) {
            if(g_mapguard_policy.disallow_transition_to_x && (prot & PROT_EXEC) && (mce->immutable_prot & PROT_WRITE)) {
                LOG("Cannot allow mapping %p to be set PROT_EXEC, it was previously PROT_WRITE", addr);
                MAYBE_PANIC
                errno = EINVAL;
                return ERROR;
            }

            if(g_mapguard_policy.disallow_transition_from_x && (prot & PROT_WRITE) && (mce->immutable_prot & PROT_EXEC)) {
                LOG("Cannot allow mapping %p to transition from PROT_EXEC to PROT_WRITE", addr);
                MAYBE_PANIC
                errno = EINVAL;
                return ERROR;
            }
        }
    }

    int32_t ret = g_real_mprotect(addr, len, prot);

    if(ret == 0 && mce) {
        /* Its possible the caller changed the protections on
         * only a portion of the mapping. Log it but ignore it */
        if(mce->size != len) {
            LOG("Cached mapping size %zu bytes but mprotected %zu bytes", mce->size, len);
        }

        /* Update the saved page permissions, even if the size doesn't match */
        mce->immutable_prot |= prot;
        mce->current_prot = prot;
    }

    return ret;
}

/* Hook mremap in libc */
void* mremap(void *__addr, size_t __old_len, size_t __new_len, int __flags, ...) {
    void *map_ptr = g_real_mremap(__addr, __old_len, __new_len, __flags);

    if(g_mapguard_policy.use_mapping_cache) {
        mapguard_cache_entry_t *mce = (mapguard_cache_entry_t *) vector_for_each(&g_map_cache_vector, (vector_for_each_callback_t *) is_mapguard_entry_cached, (void *) __addr);

        /* We are remapping a previously tracked allocation. This
         * means we may have to reallocate guard pages and update
         * the status of our cache */
        if(mce && (mce->guard_bottom || mce->guard_top) && map_ptr != MAP_FAILED) {
            g_real_munmap(mce->guard_bottom, g_page_size);
            g_real_munmap(mce->guard_top, g_page_size);
            mce->start = map_ptr;
            mce->size = __new_len;
            map_guard_pages(mce);
        }
    }

    return map_ptr;
}

#ifdef MPK_SUPPORT

/* Memory Protection Keys is a feature on newer Intel x64 Skylake processors
 * that allows a program set permission bits on a per-page mapping. The advantage
 * of MPK over mprotect() is that its a lot faster. This feature of MapGuard
 * has only been tested on AWS EC2 C5 instances and it may not even work there.
 *
 * To know if your kernel supports MPK try the following:
 * cat /proc/cpuinfo | grep -E 'pku|ospke'
 */

/* memcpy_xom - Allocates writeable memory, copies src_size bytes from src
 * into those pages, and then marks the allocation execute only. Upon
 * failure it returns MAP_FAILED. Upon success it returns a pointer to the
 * Execute Only memory region */
void *memcpy_xom(size_t allocation_size, void *src, size_t src_size) {

    if(g_mapguard_policy.use_mapping_cache == 0) {
        LOG("Cannot allocate XOM memory without MG_USE_MAPPING_CACHE enabled");
        return MAP_FAILED;
    }

    if(src == NULL || src_size == 0) {
        LOG("XOM allocation failed, src is %p and src_size = %ld", src, src_size);
        return MAP_FAILED;
    }

    if(src_size > allocation_size) {
        LOG("XOM allocation failed, src size larger than allocation size")
        return MAP_FAILED;
    }

    void *map_ptr = g_real_mmap(0x0, allocation_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);

    if(map_ptr == MAP_FAILED) {
        LOG("XOM mmap failed");
        return MAP_FAILED;
    }

    memcpy(map_ptr, src, src_size);

    mapguard_cache_entry_t *mce = (mapguard_cache_entry_t *) malloc(sizeof(mapguard_cache_entry_t));
    memset(mce, 0x0, sizeof(mapguard_cache_entry_t));

    mce->start = map_ptr;
    mce->size = allocation_size;
    mce->immutable_prot = PROT_EXEC;
    mce->current_prot = PROT_EXEC;
    mce->xom_enabled = 1;
    /* We use -1 here as a stand in for the kernels execute_only_pkey */
    mce->pkey = -1;
    mce->pkey_access_rights = PKEY_DISABLE_ACCESS;

    int32_t ret = g_real_mprotect(map_ptr, allocation_size, PROT_EXEC);

    if(ret != 0) {
        LOG("XOM mprotect failed, unmapping memory");
        g_real_munmap(map_ptr, allocation_size);
        free(mce);
        return MAP_FAILED;
    }

    mce->cache_index = vector_push(&g_map_cache_vector, mce);

    return map_ptr;
}

int munmap_xom(void *addr, size_t length) {
    mapguard_cache_entry_t *mce = (mapguard_cache_entry_t *) vector_for_each(&g_map_cache_vector, (vector_for_each_callback_t *) is_mapguard_entry_cached, (void *) addr);

    if(mce != NULL) {
        LOG("Found mapguard cache entry for mapping %p", mce->start);
        pkey_free(mce->pkey);
        vector_delete_at(&g_map_cache_vector, mce->cache_index);
        free(mce);
    } else {
        return ERROR;
    }

    return OK;
}

#endif
