# APCu-SHM: Cross-Process Shared Memory for Windows IIS

Fork of [krakjoe/apcu](https://github.com/krakjoe/apcu) with **named shared memory** support on Windows.

## Problem

APCu on Windows/IIS creates per-process anonymous memory segments. Each `php-cgi.exe` gets its own isolated cache -- no sharing, no cross-process invalidation. On Linux (FPM/Apache), APCu uses inherited `mmap` regions from `fork()`, so workers share automatically. IIS has no `fork()`.

## Solution

This fork patches APCu to use **named Windows file mappings** (`CreateFileMapping` with a name) so all `php-cgi.exe` processes serving the same IIS website share one APCu cache segment. Behavior matches Linux APCu.

```
IIS spawns php-cgi.exe #1 -> CreateFileMapping("Local\APCu_{name}_0") -> creates segment
IIS spawns php-cgi.exe #2 -> CreateFileMapping("Local\APCu_{name}_0") -> attaches to SAME segment
```

## Configuration

```ini
; Enable cross-process shared cache (Windows only)
apc.shm_name = "my_site_pool"    ; Unique name per IIS app pool / website
apc.shm_size = 32M               ; Shared memory size
apc.enabled = 1
```

When `apc.shm_name` is empty (default), APCu behaves exactly like stock -- per-process cache, fully backward-compatible.

## What Changed

All changes are `#ifdef PHP_WIN32` guarded. Linux builds are **completely unaffected**.

| Component | Change |
|-----------|--------|
| **Named shared memory** (`apc_windows_shm.c/h`) | `CreateFileMapping` with `Local\APCu_{name}` names. First process creates + initializes; others detect `ERROR_ALREADY_EXISTS` and attach. |
| **Cross-process locking** (`apc_lock.c/h`) | Replaced per-process `SRWLOCK` with atomics-based rwlock stored IN shared memory. Uses `InterlockedCompareExchange` / `InterlockedIncrement`. Crash recovery via PID tracking. |
| **Security** (`apc_windows_security.c/h`) | DACL on file mapping restricted to the current process identity. Different IIS app pools can't access each other's segments. |
| **SMA attach** (`apc_sma.c/h`) | New `apc_sma_attach()` path: skips free-list initialization when joining existing segment. |
| **Cache attach** (`apc_cache.c/h`) | New `apc_cache_attach()`: wraps existing shared cache header with per-process descriptor. |
| **INI** (`php_apc.c`, `apc_globals.h`) | New `apc.shm_name` directive (PHP_INI_SYSTEM). |
| **Build** (`config.w32`) | Updated source list, removed obsolete SRWLOCK kernel module. |

## Process Lifecycle

```
IIS starts app pool
  -> spawns php-cgi.exe #1
    -> MINIT: acquires init mutex, CreateFileMapping (NEW), init SMA + cache
    -> releases init mutex
    -> serves requests, reads/writes shared cache

  -> spawns php-cgi.exe #2
    -> MINIT: acquires init mutex, CreateFileMapping (EXISTING), attach SMA + cache
    -> releases init mutex
    -> sees all keys from process #1 immediately

  -> php-cgi.exe #1 crashes
    -> segment persists (process #2 still holds a handle)
    -> stale write lock detected via PID check, auto-reset

  -> IIS recycles app pool
    -> all processes exit, all handles close
    -> Windows destroys the named segment automatically
    -> next request: fresh segment, clean cache
```

## Build

```powershell
# In PHP build tree:
cd pecl\apcu
phpize
configure --enable-apcu
nmake

# Output: Release_NTS\php_apcu.dll
```

Target: PHP 8.4+ NTS on Windows/IIS FastCGI.

## Verification

1. **Basic sharing**: Process A `apcu_store('k','v')`, Process B `apcu_fetch('k')` returns `'v'`
2. **Invalidation**: Process A `apcu_delete('k')`, Process B sees it gone
3. **Concurrent writes**: 10 processes writing different keys -- no corruption
4. **Crash recovery**: `taskkill /f` a process mid-write, others don't deadlock
5. **App pool recycle**: Cache is empty after restart (segment destroyed + recreated)
6. **Isolation**: Different `apc.shm_name` values = no cross-contamination
7. **Backward compat**: Empty `apc.shm_name` = stock per-process behavior

## Credits

- Original APCu by [krakjoe](https://github.com/krakjoe/apcu) and contributors
- Named shared memory patch by [Lakeworks](https://github.com/lakeworks)

## License

PHP License 3.01 (same as upstream APCu)
