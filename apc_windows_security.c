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

#ifdef PHP_WIN32

#include "apc_windows_security.h"
#include "apc.h"

int apc_windows_build_dacl(SECURITY_ATTRIBUTES *sa, apc_windows_sd_t *sd_out)
{
	HANDLE token = NULL;
	TOKEN_USER *user = NULL;
	DWORD user_size = 0;
	PACL dacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	EXPLICIT_ACCESS_A ea;
	DWORD result;

	if (!sa || !sd_out) {
		return 0;
	}

	sd_out->sd = NULL;
	sd_out->dacl = NULL;

	/* Get the current process token */
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		apc_warning("apc_windows_build_dacl: OpenProcessToken failed: %lu", GetLastError());
		return 0;
	}

	/* Get required size for TOKEN_USER */
	GetTokenInformation(token, TokenUser, NULL, 0, &user_size);
	if (user_size == 0) {
		apc_warning("apc_windows_build_dacl: GetTokenInformation size query failed: %lu", GetLastError());
		CloseHandle(token);
		return 0;
	}

	user = (TOKEN_USER *)malloc(user_size);
	if (!user) {
		CloseHandle(token);
		return 0;
	}

	if (!GetTokenInformation(token, TokenUser, user, user_size, &user_size)) {
		apc_warning("apc_windows_build_dacl: GetTokenInformation failed: %lu", GetLastError());
		free(user);
		CloseHandle(token);
		return 0;
	}

	/* Build an ACE granting full access to the current user only */
	memset(&ea, 0, sizeof(ea));
	ea.grfAccessPermissions = FILE_MAP_ALL_ACCESS | SECTION_ALL_ACCESS;
	ea.grfAccessMode = SET_ACCESS;
	ea.grfInheritance = NO_INHERITANCE;
	ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
	ea.Trustee.ptstrName = (LPSTR)user->User.Sid;

	result = SetEntriesInAclA(1, &ea, NULL, &dacl);
	if (result != ERROR_SUCCESS) {
		apc_warning("apc_windows_build_dacl: SetEntriesInAcl failed: %lu", result);
		free(user);
		CloseHandle(token);
		return 0;
	}

	/* Build security descriptor */
	sd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	if (!sd) {
		LocalFree(dacl);
		free(user);
		CloseHandle(token);
		return 0;
	}

	if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION)) {
		LocalFree(sd);
		LocalFree(dacl);
		free(user);
		CloseHandle(token);
		return 0;
	}

	if (!SetSecurityDescriptorDacl(sd, TRUE, dacl, FALSE)) {
		LocalFree(sd);
		LocalFree(dacl);
		free(user);
		CloseHandle(token);
		return 0;
	}

	/* Fill in SECURITY_ATTRIBUTES */
	sa->nLength = sizeof(SECURITY_ATTRIBUTES);
	sa->lpSecurityDescriptor = sd;
	sa->bInheritHandle = FALSE;

	/* Track both pointers for safe cleanup */
	sd_out->sd = sd;
	sd_out->dacl = dacl;

	free(user);
	CloseHandle(token);

	return 1;
}

void apc_windows_free_dacl(apc_windows_sd_t *sd_info)
{
	if (!sd_info) {
		return;
	}

	if (sd_info->dacl) {
		LocalFree(sd_info->dacl);
		sd_info->dacl = NULL;
	}

	if (sd_info->sd) {
		LocalFree(sd_info->sd);
		sd_info->sd = NULL;
	}
}

#endif /* PHP_WIN32 */
