#ifndef TGLIB_MMAP_H
#define TGLIB_MMAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* for size_t */

/* Function Declarations */

/**
 * Initialize the mmap tracking system.
 * Reads environment variable VVV_MAX_MMAP_CALLS (default 32).
 */
void tglib_init_mmap_info(void);

/**
 * Free all tracked memory mappings and cleanup
 * 
 * Unmaps all memory regions tracked via tglib_add_mmap_info() and
 * frees the tracking array.
 */
void tglib_free_mmap_info(void);

/**
 * Print memory policy information for debugging
 * 
 * Prints thread memory policy and optionally address-specific policy.
 * Output includes process ID, thread ID, policy type, and nodemask.
 * 
 * @param prefix Descriptive prefix for the output
 * @param addr Optional address to check policy for (can be NULL)
 * @param length Length of memory region in bytes (use 0 if addr is NULL)
 */
void tglib_print_mempolicy(const char* prefix, void* addr, size_t length);

/**
 * Allocate memory using mmap with NUMA local policy
 * 
 * Allocates memory using mmap with MAP_ANONYMOUS and applies MPOL_LOCAL
 * memory policy for NUMA locality. Automatically tracks the mapping.
 * 
 * @param length Size of memory to allocate in bytes
 * @return Pointer to allocated memory
 * 
 * @note Aborts on failure (mmap or mbind failure)
 * @note Memory is automatically tracked and freed by tglib_destroy()
 * @note Prints memory policy information before and after mbind
 */
void *tglib_mmap_wrapper(size_t length);

/**
 * Get the total memory reserved through tglib_mmap_wrapper
 * 
 * @return Total bytes of memory reserved across all tglib_mmap_wrapper() calls
 */
size_t tglib_get_total_memory_reserved(void);

#ifdef __cplusplus
}
#endif

#endif /* TGLIB_MMAP_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <numaif.h>
#include <pthread.h>
#include <unistd.h>
#include <linux/mman.h>
#include <sys/types.h>

/* Internal structures */
struct mmap_info {
	void *addr;
	size_t length;
    int is_mmap; /* 1 if allocated via mmap, 0 if via malloc */
};

struct arr_mmap_info {
	struct mmap_info *arr;
	size_t size;
	size_t capacity;
};

/* Global state for the implementation unit */
static struct arr_mmap_info mmap_infos;
static size_t total_memory_reserved = 0;
static int g_mmap_enabled = 1;

void tglib_init_mmap_info(void) {
	const char* env_max = getenv("VVV_MAX_MMAP_CALLS");
	int max_size = 32;
	if (env_max) {
		max_size = atoi(env_max);
		if (max_size <= 0) {
			fprintf(stderr, "Invalid VVV_MAX_MMAP_CALLS value: %s\n", env_max);
			abort();
		}
	}
	const char* env_mmap = getenv("VVV_MMAP_ENABLED");
	if (env_mmap && env_mmap[0] == '1') {
		g_mmap_enabled = 1;
	} else {
		g_mmap_enabled = 0;
	}

	fprintf(stderr, "tglib: allocation mode = %s\n", g_mmap_enabled ? "mmap" : "malloc");
	mmap_infos.arr = (struct mmap_info*) malloc(max_size * sizeof(struct mmap_info));
	if (!mmap_infos.arr) {
		fprintf(stderr, "Failed to allocate memory for mmap_infos\n");
		abort();
	}
	mmap_infos.capacity = max_size;
	mmap_infos.size = 0;
}

void tglib_add_mmap_info(void *addr, size_t length) {
	if (mmap_infos.size >= mmap_infos.capacity) {
		fprintf(stderr, "Exceeded max mmap info capacity!\n");
		abort();
	}
	mmap_infos.arr[mmap_infos.size].addr = addr;
	mmap_infos.arr[mmap_infos.size].length = length;
	mmap_infos.arr[mmap_infos.size].is_mmap = g_mmap_enabled;
	mmap_infos.size++;
}

void tglib_free_mmap_info(void) {
	for (size_t i = 0; i < mmap_infos.size; i++) {
		if (mmap_infos.arr[i].is_mmap) {
			munmap(mmap_infos.arr[i].addr, mmap_infos.arr[i].length);
		} else {
			free(mmap_infos.arr[i].addr);
		}
	}
	free(mmap_infos.arr);
	mmap_infos.arr = NULL;
	mmap_infos.size = 0;
	mmap_infos.capacity = 0;
}

void tglib_print_mempolicy(const char* prefix, void* addr, size_t length) {
	pid_t procid = getpid();
	
	int policy_thread = -1;
	unsigned long nodemask_thread = 0;
	int ret_thread = get_mempolicy(&policy_thread, &nodemask_thread, sizeof(nodemask_thread) * 8, NULL, 0);
	if (ret_thread != 0) {
		fprintf(stderr, "get_mempolicy failed for thread!\n");
		abort();
	}
	
	fprintf(stderr, "[Proc %d, Thread %lu] %s: thread_policy=%d thread_nodemask=0x%lx",
	        procid, (unsigned long)pthread_self(), prefix, policy_thread, nodemask_thread);
	
	if (addr != NULL && length > 0) {
		int policy_addr = -1;
		unsigned long nodemask_addr = 0;
		int ret_addr = get_mempolicy(&policy_addr, &nodemask_addr, sizeof(nodemask_addr) * 8, addr, MPOL_F_ADDR);
		if (ret_addr != 0) {
			fprintf(stderr, "get_mempolicy failed for addr!\n");
			abort();
		}
		
		fprintf(stderr, " addr_policy=%d addr_nodemask=0x%lx", policy_addr, nodemask_addr);
	}
	
	fprintf(stderr, "\n");
}

void *tglib_mmap_wrapper(size_t length) {
	void *addr = NULL;
	if (g_mmap_enabled) {
		addr = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (addr == MAP_FAILED) {
			fprintf(stderr, "mmap failed for length %zu\n", length);
			abort();
		}

		tglib_print_mempolicy("BEFORE mbind", addr, length);

		int ret = mbind(addr, length, MPOL_LOCAL, NULL, 0, MPOL_MF_STRICT);
		if (ret != 0) {
			fprintf(stderr, "mbind failed!\n");
			abort();
		}

		tglib_print_mempolicy("AFTER  mbind", addr, length);
	} else {
		addr = malloc(length);
		if (!addr) {
			fprintf(stderr, "malloc failed for length %zu\n", length);
			abort();
		}
		tglib_print_mempolicy("ALLOC malloc", addr, length);
	}

	total_memory_reserved += length;
	tglib_add_mmap_info(addr, length);
	return addr;
}

size_t tglib_get_total_memory_reserved(void) {
	return total_memory_reserved;
}

