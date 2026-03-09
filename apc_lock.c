/*
  +----------------------------------------------------------------------+
  | APCu                                                                 |
  +----------------------------------------------------------------------+
  | Copyright (c) 2013 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Joe Watkins <joe.watkins@live.co.uk>                        |
  +----------------------------------------------------------------------+
 */

#ifndef HAVE_APC_LOCK_H
# include "apc_lock.h"
#endif

/*
 * While locking calls should never fail, apcu checks for the success of write-lock
 * acquisitions, to prevent more damage when a deadlock is detected.
 */

#ifdef PHP_WIN32

/*
 * Cross-process atomics-based rwlock for Windows.
 *
 * The lock struct (apc_lock_t) lives IN the shared memory segment, so
 * multiple php-cgi.exe processes can synchronize via Interlocked* operations.
 *
 * Read lock:  multiple concurrent readers allowed
 * Write lock: exclusive, with crash recovery via PID tracking
 *
 * Spin strategy: brief YieldProcessor(), then SwitchToThread() for longer waits.
 */

/* Maximum spin iterations before checking for dead writer or forcing reset */
#define APC_LOCK_SPIN_YIELD     64
#define APC_LOCK_SPIN_CRASH_CHECK 1024
#define APC_LOCK_SPIN_MAX       2000000

PHP_APCU_API zend_bool apc_lock_init() {
	return 1;
}

PHP_APCU_API void apc_lock_cleanup() {
}

PHP_APCU_API zend_bool apc_lock_create(apc_lock_t *lock) {
	lock->readers = 0;
	lock->writer = 0;
	lock->writer_pid = 0;
	return 1;
}

static inline zend_bool apc_lock_rlock_impl(apc_lock_t *lock) {
	int spins = 0;
	for (;;) {
		/* Wait until no writer is active */
		while (lock->writer) {
			if (++spins < APC_LOCK_SPIN_YIELD) {
				YieldProcessor();
			} else {
				SwitchToThread();
			}
		}

		/* Increment reader count.
		 * InterlockedIncrement has full barrier semantics on x86/x64,
		 * providing the acquire ordering we need to see the writer's
		 * data modifications before proceeding. */
		InterlockedIncrement(&lock->readers);

		/* Double-check: if a writer snuck in, back off and retry */
		if (!lock->writer) {
			return 1;
		}

		InterlockedDecrement(&lock->readers);
	}
}

static inline zend_bool apc_lock_wlock_impl(apc_lock_t *lock) {
	DWORD my_pid = GetCurrentProcessId();
	int spins = 0;

	for (;;) {
		/* Try to acquire the writer flag */
		if (InterlockedCompareExchange(&lock->writer, 1, 0) == 0) {
			/* We are the writer */
			lock->writer_pid = my_pid;

			/* Wait for all readers to drain */
			while (lock->readers) {
				if (++spins < APC_LOCK_SPIN_YIELD) {
					YieldProcessor();
				} else {
					SwitchToThread();
				}

				/* Safety: if readers are stuck for too long, force-reset.
				 * This handles the case where a reader process died. */
				if (spins > APC_LOCK_SPIN_MAX) {
					InterlockedExchange(&lock->readers, 0);
					break;
				}
			}
			/* Acquire barrier: ensure we see all prior writes from
			 * other processes before we start modifying shared data. */
			MemoryBarrier();
			return 1;
		}

		/* Another writer holds the lock. Periodically check if it's a dead process.
		 * OpenProcess is an expensive syscall, so only check every 1024 spins
		 * to avoid kernel overhead under contention. */
		++spins;
		if (spins >= APC_LOCK_SPIN_CRASH_CHECK && (spins & 0x3FF) == 0) {
			DWORD holder_pid = lock->writer_pid;
			if (holder_pid != 0 && holder_pid != my_pid) {
				HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, holder_pid);
				if (!hProc) {
					/* Process doesn't exist anymore - force-reset the lock */
					InterlockedExchange(&lock->writer, 0);
					lock->writer_pid = 0;
					spins = 0;
					continue;
				} else {
					DWORD exit_code;
					if (GetExitCodeProcess(hProc, &exit_code) && exit_code != STILL_ACTIVE) {
						CloseHandle(hProc);
						InterlockedExchange(&lock->writer, 0);
						lock->writer_pid = 0;
						spins = 0;
						continue;
					}
					CloseHandle(hProc);
				}
			}
		}

		/* Force-reset after excessive spinning (guards against PID reuse edge case) */
		if (spins > APC_LOCK_SPIN_MAX) {
			InterlockedExchange(&lock->writer, 0);
			lock->writer_pid = 0;
			spins = 0;
			continue;
		}

		if (spins < APC_LOCK_SPIN_YIELD) {
			YieldProcessor();
		} else {
			SwitchToThread();
		}
	}
}

PHP_APCU_API zend_bool apc_lock_wunlock(apc_lock_t *lock) {
	/* Release barrier: ensure all data writes performed under the write lock
	 * are visible to other processors BEFORE we clear the writer flag.
	 * This is critical for cross-process correctness. */
	MemoryBarrier();
	lock->writer_pid = 0;
	/* InterlockedExchange has full barrier semantics on x86/x64,
	 * ensuring the writer_pid=0 and all prior stores are visible
	 * before the writer flag is cleared. */
	InterlockedExchange(&lock->writer, 0);
	return 1;
}

PHP_APCU_API zend_bool apc_lock_runlock(apc_lock_t *lock) {
	InterlockedDecrement(&lock->readers);
	return 1;
}

PHP_APCU_API void apc_lock_destroy(apc_lock_t *lock) {
	/* Nothing to destroy - atomics don't need cleanup */
}

#elif defined(APC_NATIVE_RWLOCK)

static zend_bool apc_lock_ready = 0;
static pthread_rwlockattr_t apc_lock_attr;

PHP_APCU_API zend_bool apc_lock_init() {
	if (apc_lock_ready) {
		return 1;
	}
	apc_lock_ready = 1;

	if (pthread_rwlockattr_init(&apc_lock_attr) != SUCCESS) {
		return 0;
	}
	if (pthread_rwlockattr_setpshared(&apc_lock_attr, PTHREAD_PROCESS_SHARED) != SUCCESS) {
		return 0;
	}
	return 1;
}

PHP_APCU_API void apc_lock_cleanup() {
	if (!apc_lock_ready) {
		return;
	}
	apc_lock_ready = 0;

	pthread_rwlockattr_destroy(&apc_lock_attr);
}

PHP_APCU_API zend_bool apc_lock_create(apc_lock_t *lock) {
	return pthread_rwlock_init(lock, &apc_lock_attr) == SUCCESS;
}

static inline zend_bool apc_lock_rlock_impl(apc_lock_t *lock) {
	return pthread_rwlock_rdlock(lock) == 0;
}

static inline zend_bool apc_lock_wlock_impl(apc_lock_t *lock) {
	return pthread_rwlock_wrlock(lock) == 0;
}

PHP_APCU_API zend_bool apc_lock_wunlock(apc_lock_t *lock) {
	pthread_rwlock_unlock(lock);
	return 1;
}

PHP_APCU_API zend_bool apc_lock_runlock(apc_lock_t *lock) {
	pthread_rwlock_unlock(lock);
	return 1;
}

PHP_APCU_API void apc_lock_destroy(apc_lock_t *lock) {
	pthread_rwlock_destroy(lock);
}

#elif defined(APC_LOCK_RECURSIVE)

static zend_bool apc_lock_ready = 0;
static pthread_mutexattr_t apc_lock_attr;

PHP_APCU_API zend_bool apc_lock_init() {
	if (apc_lock_ready) {
		return 1;
	}
	apc_lock_ready = 1;

	if (pthread_mutexattr_init(&apc_lock_attr) != SUCCESS) {
		return 0;
	}

	if (pthread_mutexattr_setpshared(&apc_lock_attr, PTHREAD_PROCESS_SHARED) != SUCCESS) {
		return 0;
	}

	pthread_mutexattr_settype(&apc_lock_attr, PTHREAD_MUTEX_RECURSIVE);
	return 1;
}

PHP_APCU_API void apc_lock_cleanup() {
	if (!apc_lock_ready) {
		return;
	}
	apc_lock_ready = 0;

	pthread_mutexattr_destroy(&apc_lock_attr);
}

PHP_APCU_API zend_bool apc_lock_create(apc_lock_t *lock) {
	pthread_mutex_init(lock, &apc_lock_attr);
	return 1;
}

static inline zend_bool apc_lock_rlock_impl(apc_lock_t *lock) {
	return pthread_mutex_lock(lock) == 0;
}

static inline zend_bool apc_lock_wlock_impl(apc_lock_t *lock) {
	return pthread_mutex_lock(lock) == 0;
}

PHP_APCU_API zend_bool apc_lock_wunlock(apc_lock_t *lock) {
	pthread_mutex_unlock(lock);
	return 1;
}

PHP_APCU_API zend_bool apc_lock_runlock(apc_lock_t *lock) {
	pthread_mutex_unlock(lock);
	return 1;
}

PHP_APCU_API void apc_lock_destroy(apc_lock_t *lock) {
	pthread_mutex_destroy(lock);
}

#elif defined(APC_SPIN_LOCK)

static int apc_lock_try(apc_lock_t *lock) {
	int failed = 1;

	asm volatile
	(
		"xchgl %0, 0(%1)" :
		"=r" (failed) : "r" (&lock->state),
		"0" (failed)
	);

	return failed;
}

static int apc_lock_get(apc_lock_t *lock) {
	int failed = 1;

	do {
		failed = apc_lock_try(lock);
#ifdef APC_LOCK_NICE
		usleep(0);
#endif
	} while (failed);

	return failed;
}

static int apc_lock_release(apc_lock_t *lock) {
	int released = 0;

	asm volatile (
		"xchg %0, 0(%1)" : "=r" (released) : "r" (&lock->state),
		"0" (released)
	);

	return !released;
}

PHP_APCU_API zend_bool apc_lock_init() {
	return 0;
}

PHP_APCU_API void apc_lock_cleanup() {
}

PHP_APCU_API zend_bool apc_lock_create(apc_lock_t *lock) {
	lock->state = 0;
}

static inline zend_bool apc_lock_rlock_impl(apc_lock_t *lock) {
	apc_lock_get(lock);
	return 1;
}

static inline zend_bool apc_lock_wlock_impl(apc_lock_t *lock) {
	apc_lock_get(lock);
	return 1;
}

PHP_APCU_API zend_bool apc_lock_wunlock(apc_lock_t *lock) {
	apc_lock_release(lock);
	return 1;
}

PHP_APCU_API zend_bool apc_lock_runlock(apc_lock_t *lock) {
	apc_lock_release(lock);
	return 1;
}

PHP_APCU_API void apc_lock_destroy(apc_lock_t *lock) {
}

#else

#include <unistd.h>
#include <fcntl.h>

static int apc_fcntl_call(int fd, int cmd, int type, off_t offset, int whence, off_t len) {
	int ret;
	struct flock lock;

	lock.l_type = type;
	lock.l_start = offset;
	lock.l_whence = whence;
	lock.l_len = len;
	lock.l_pid = 0;

	do {
		ret = fcntl(fd, cmd, &lock) ;
	} while(ret < 0 && errno == EINTR);

	return(ret);
}

PHP_APCU_API zend_bool apc_lock_init() {
	return 0;
}

PHP_APCU_API void apc_lock_cleanup() {
}

PHP_APCU_API zend_bool apc_lock_create(apc_lock_t *lock) {
	char lock_path[] = "/tmp/.apc.XXXXXX";

	*lock = mkstemp(lock_path);
	if (*lock > 0) {
		unlink(lock_path);
		return 1;
	} else {
		return 0;
	}
}

static inline zend_bool apc_lock_rlock_impl(apc_lock_t *lock) {
	apc_fcntl_call((*lock), F_SETLKW, F_RDLCK, 0, SEEK_SET, 0);
	return 1;
}

static inline zend_bool apc_lock_wlock_impl(apc_lock_t *lock) {
	apc_fcntl_call((*lock), F_SETLKW, F_WRLCK, 0, SEEK_SET, 0);
	return 1;
}

PHP_APCU_API zend_bool apc_lock_wunlock(apc_lock_t *lock) {
	apc_fcntl_call((*lock), F_SETLKW, F_UNLCK, 0, SEEK_SET, 0);
	return 1;
}

PHP_APCU_API zend_bool apc_lock_runlock(apc_lock_t *lock) {
	apc_fcntl_call((*lock), F_SETLKW, F_UNLCK, 0, SEEK_SET, 0);
	return 1;
}

PHP_APCU_API void apc_lock_destroy(apc_lock_t *lock) {
	close(*lock);
}

#endif

/* Shared for all lock implementations */

PHP_APCU_API zend_bool apc_lock_wlock(apc_lock_t *lock) {
	HANDLE_BLOCK_INTERRUPTIONS();
	if (apc_lock_wlock_impl(lock)) {
		return 1;
	}

	HANDLE_UNBLOCK_INTERRUPTIONS();
	apc_warning("Failed to acquire write lock");
	return 0;
}

PHP_APCU_API zend_bool apc_lock_rlock(apc_lock_t *lock) {
	HANDLE_BLOCK_INTERRUPTIONS();
	if (apc_lock_rlock_impl(lock)) {
		return 1;
	}

	HANDLE_UNBLOCK_INTERRUPTIONS();
	apc_warning("Failed to acquire read lock");
	return 0;
}
