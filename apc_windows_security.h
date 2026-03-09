/*
  +----------------------------------------------------------------------+
  | APCu - Windows Security (DACL for app pool isolation)                |
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

#ifndef APC_WINDOWS_SECURITY_H
#define APC_WINDOWS_SECURITY_H

#ifdef PHP_WIN32

#include <windows.h>
#include <aclapi.h>

/*
 * Wrapper holding both the security descriptor and DACL pointers,
 * so cleanup doesn't need to extract the DACL back from the SD.
 */
typedef struct _apc_windows_sd_t {
	PSECURITY_DESCRIPTOR sd;
	PACL dacl;
} apc_windows_sd_t;

/*
 * Build a SECURITY_ATTRIBUTES with a DACL restricted to the current
 * process identity (the IIS app pool user). This ensures that shared
 * memory segments created for one app pool cannot be accessed by
 * processes running as a different identity.
 *
 * On success, stores an apc_windows_sd_t* in sa->lpSecurityDescriptor's
 * companion field. The caller must call apc_windows_free_dacl() to release.
 *
 * Returns 1 on success, 0 on failure (sa is left unmodified on failure).
 */
int apc_windows_build_dacl(SECURITY_ATTRIBUTES *sa, apc_windows_sd_t *sd_out);

/*
 * Free resources allocated by apc_windows_build_dacl().
 */
void apc_windows_free_dacl(apc_windows_sd_t *sd_info);

#endif /* PHP_WIN32 */
#endif /* APC_WINDOWS_SECURITY_H */
