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

#ifdef PHP_WIN32

#include "apc_windows_shm.h"
#include "apc_windows_security.h"

/* Maximum shm_name length to prevent buffer overflow in name formatting.
 * Names are formatted as "Local\APCu_{name}_init" (longest suffix), so
 * 200 chars leaves room for the ~20-char prefix/suffix in a 256-byte buffer. */
#define APC_SHM_NAME_MAX 200

/* Validate that shm_name contains only safe characters for Windows named objects.
 * Allows alphanumeric, underscore, hyphen, and dot. */
static int apc_windows_shm_validate_name(const char *name) {
	const char *p;
	if (!name || !name[0]) {
		return 0;
	}
	if (strlen(name) > APC_SHM_NAME_MAX) {
		return 0;
	}
	for (p = name; *p; p++) {
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) {
			return 0;
		}
	}
	return 1;
}

HANDLE apc_windows_shm_init_lock(const char *shm_name)
{
	char mutex_name[256];
	HANDLE mutex;

	if (!apc_windows_shm_validate_name(shm_name)) {
		zend_error_noreturn(E_CORE_ERROR,
			"apc_windows_shm_init_lock: shm_name is invalid (must be 1-%d chars, alphanumeric/underscore/hyphen/dot only)",
			APC_SHM_NAME_MAX);
		return NULL;
	}

	snprintf(mutex_name, sizeof(mutex_name), "Local\\APCu_%s_init", shm_name);

	mutex = CreateMutexA(NULL, FALSE, mutex_name);
	if (!mutex) {
		zend_error_noreturn(E_CORE_ERROR,
			"apc_windows_shm_init_lock: CreateMutex(%s) failed: %lu",
			mutex_name, GetLastError());
		return NULL;
	}

	DWORD wait_result = WaitForSingleObject(mutex, 30000); /* 30 second timeout */
	if (wait_result == WAIT_FAILED) {
		CloseHandle(mutex);
		zend_error_noreturn(E_CORE_ERROR,
			"apc_windows_shm_init_lock: WaitForSingleObject failed: %lu",
			GetLastError());
		return NULL;
	}
	if (wait_result == WAIT_TIMEOUT) {
		CloseHandle(mutex);
		zend_error_noreturn(E_CORE_ERROR,
			"apc_windows_shm_init_lock: Timed out waiting for init mutex '%s'. "
			"Another process may be stuck during initialization.", mutex_name);
		return NULL;
	}
	/* WAIT_OBJECT_0 = acquired normally
	 * WAIT_ABANDONED = previous holder died, we now own it (acceptable) */

	return mutex;
}

void apc_windows_shm_init_unlock(HANDLE init_mutex)
{
	if (init_mutex) {
		ReleaseMutex(init_mutex);
		CloseHandle(init_mutex);
	}
}

apc_windows_shm_t *apc_windows_shm_create(const char *shm_name, size_t size)
{
	char mapping_name[256];
	HANDLE mapping;
	void *addr;
	BOOL is_new;
	SECURITY_ATTRIBUTES sa;
	SECURITY_ATTRIBUTES *psa = NULL;
	apc_windows_sd_t sd_info = {0};
	DWORD size_high, size_low;

	if (!apc_windows_shm_validate_name(shm_name)) {
		apc_error("apc_windows_shm_create: shm_name is invalid (must be 1-%d chars, alphanumeric/underscore/hyphen/dot only)",
			APC_SHM_NAME_MAX);
		return NULL;
	}

	snprintf(mapping_name, sizeof(mapping_name), "Local\\APCu_%s_0", shm_name);

	/* Build DACL for app pool isolation */
	if (apc_windows_build_dacl(&sa, &sd_info)) {
		psa = &sa;
	}

	/* Split size into high/low DWORDs for CreateFileMapping */
#ifdef _WIN64
	size_high = (DWORD)(size >> 32);
	size_low = (DWORD)(size & 0xFFFFFFFF);
#else
	size_high = 0;
	size_low = (DWORD)size;
#endif

	mapping = CreateFileMappingA(
		INVALID_HANDLE_VALUE,   /* page-file backed */
		psa,                    /* security attributes */
		PAGE_READWRITE,         /* read/write access */
		size_high,              /* size high DWORD */
		size_low,               /* size low DWORD */
		mapping_name            /* named mapping */
	);

	/* Capture GetLastError() immediately — apc_windows_free_dacl() below
	 * calls LocalFree() which may reset it to 0 on success. */
	{
		DWORD create_err = GetLastError();
		is_new = (create_err != ERROR_ALREADY_EXISTS);

		/* Free DACL resources regardless of CreateFileMapping success */
		apc_windows_free_dacl(&sd_info);

		if (!mapping) {
			apc_error("apc_windows_shm_create: CreateFileMapping(%s, %zu bytes) failed: %lu",
				mapping_name, size, create_err);
			return NULL;
		}
	}

	addr = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (!addr) {
		DWORD err = GetLastError();
		CloseHandle(mapping);
		apc_error("apc_windows_shm_create: MapViewOfFile(%s) failed: %lu",
			mapping_name, err);
		return NULL;
	}

	apc_windows_shm_t *shm = pemalloc(sizeof(apc_windows_shm_t), 1);
	shm->mapping_handle = mapping;
	shm->addr = addr;
	shm->size = size;
	shm->is_new = is_new;

	return shm;
}

void apc_windows_shm_detach(apc_windows_shm_t *shm)
{
	if (!shm) {
		return;
	}

	if (shm->addr) {
		UnmapViewOfFile(shm->addr);
		shm->addr = NULL;
	}

	if (shm->mapping_handle) {
		CloseHandle(shm->mapping_handle);
		shm->mapping_handle = NULL;
	}

	pefree(shm, 1);
}

#endif /* PHP_WIN32 */
