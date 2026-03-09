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
 * Build a SECURITY_ATTRIBUTES with a DACL restricted to the current
 * process identity (the IIS app pool user). This ensures that shared
 * memory segments created for one app pool cannot be accessed by
 * processes running as a different identity.
 *
 * Returns 1 on success, 0 on failure (sa is left unmodified on failure).
 * On success, the caller must call apc_windows_free_dacl() to release
 * allocated resources.
 */
int apc_windows_build_dacl(SECURITY_ATTRIBUTES *sa);

/*
 * Free resources allocated by apc_windows_build_dacl().
 */
void apc_windows_free_dacl(SECURITY_ATTRIBUTES *sa);

#endif /* PHP_WIN32 */
#endif /* APC_WINDOWS_SECURITY_H */
