/*
  +----------------------------------------------------------------------+
  | APCu - Windows Named Shared Memory                                   |
  +----------------------------------------------------------------------+
  | Copyright (c) 2026 Lakeworks                                         |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  +----------------------------------------------------------------------+
  | Authors: Arnaud Music <arnaud@lakeworks.fr>                          |
  +----------------------------------------------------------------------+
 */

#ifndef APC_WINDOWS_SHM_H
#define APC_WINDOWS_SHM_H

#ifdef PHP_WIN32

#include "apc.h"

typedef struct _apc_windows_shm_t {
	HANDLE mapping_handle;    /* handle to the file mapping object */
	void *addr;               /* mapped view base address */
	size_t size;              /* size of the mapping */
	zend_bool is_new;         /* TRUE if this process created the segment */
} apc_windows_shm_t;

/*
 * Acquire the per-name initialization mutex.
 * Must be called before apc_windows_shm_create() and held until all
 * shared structures (SMA, cache) are fully initialized or attached.
 * Returns the mutex handle (must be passed to apc_windows_shm_init_unlock).
 */
HANDLE apc_windows_shm_init_lock(const char *shm_name);

/*
 * Release the initialization mutex.
 */
void apc_windows_shm_init_unlock(HANDLE init_mutex);

/*
 * Create or attach to a named shared memory segment.
 *
 * If the named segment already exists, this attaches to it (is_new = FALSE).
 * If it does not exist, this creates it (is_new = TRUE).
 *
 * The caller is responsible for:
 *   - Holding the init mutex during the call
 *   - Initializing shared structures only when is_new == TRUE
 *
 * Returns NULL on failure.
 * The returned struct is allocated with pemalloc(,1) and must be freed
 * with apc_windows_shm_detach().
 */
apc_windows_shm_t *apc_windows_shm_create(const char *shm_name, size_t size);

/*
 * Detach from the shared memory segment and close handles.
 * The underlying segment persists as long as at least one process holds a handle.
 */
void apc_windows_shm_detach(apc_windows_shm_t *shm);

#endif /* PHP_WIN32 */
#endif /* APC_WINDOWS_SHM_H */
