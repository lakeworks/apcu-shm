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
; php.ini - Enable cross-process shared cache (Windows only)
apc.shm_name = "my_site_pool"       ; Unique name per IIS app pool / website
apc.shm_size = 32M                  ; Shared memory size
apc.enabled = 1
apc.windows_shared_only = 1         ; Disable APCu if shm_name is not set (recommended)
```

### Directives

| Directive | Type | Default | Description |
|-----------|------|---------|-------------|
| `apc.shm_name` | string | `""` | Named shared memory segment identifier. Processes with the same name share one cache. If empty, falls back to per-process anonymous memory. |
| `apc.windows_shared_only` | bool | `0` | When enabled, APCu is disabled if `apc.shm_name` is not set. Prevents accidental per-process mode which wastes memory and breaks invalidation. |

Both directives are `PHP_INI_SYSTEM` (php.ini only, not per-directory).

### Multi-site / multi-pool

Each IIS application pool should use a unique `apc.shm_name`. Configure per-pool `php.ini` files:

```
Pool "site_a" -> php.ini: apc.shm_name = "site_a"
Pool "site_b" -> php.ini: apc.shm_name = "site_b"
```

Isolation is enforced at two levels:
1. **Naming**: different names = different segments
2. **DACL**: OS-level access control blocks cross-pool access even if names collide

## What Changed

All changes are `#ifdef PHP_WIN32` guarded. Linux builds are **completely unaffected**.

| Component | Change |
|-----------|--------|
| **Named shared memory** (`apc_windows_shm.c/h`) | `CreateFileMapping` with `Local\APCu_{name}` names. First process creates + initializes; others detect `ERROR_ALREADY_EXISTS` and attach. `VirtualQuery` validates actual mapped region size. |
| **Cross-process locking** (`apc_lock.c/h`) | Replaced per-process `SRWLOCK` with atomics-based rwlock stored IN shared memory. Uses `InterlockedCompareExchange` for CAS-based crash recovery via PID tracking. Only dead processes get locks revoked (`apc_lock_process_alive()` check); live slow writers are waited on. Writer aborts on reader drain timeout (no force-reset). |
| **Security** (`apc_windows_security.c/h`) | DACL on both file mapping and init mutex, restricted to current process identity (fail-closed). Least-privilege permissions: `FILE_MAP_ALL_ACCESS \| MUTEX_ALL_ACCESS \| SYNCHRONIZE`. |
| **SMA attach** (`apc_sma.c/h`) | New `apc_sma_attach()` path: skips free-list initialization when joining existing segment. ABI version guard (`APC_SHM_ABI_VERSION`) checked first on attach. Serializer name persisted in shared header for cross-process validation. Validates `seg_size` against actual mapped region, caps `max_alloc_size`. |
| **Cache attach** (`apc_cache.c/h`) | New `apc_cache_attach()`: wraps existing shared cache header with per-process descriptor. Overflow-safe bounds check on `nslots`. |
| **INI** (`php_apc.c`, `apc_globals.h`) | New `apc.shm_name` and `apc.windows_shared_only` directives (PHP_INI_SYSTEM). |
| **Build** (`config.w32`) | Updated source list, removed obsolete SRWLOCK kernel module. |

## Security Model

- **`Local\` namespace**: segments scoped to the Windows logon session -- different sessions can't collide
- **DACL**: explicit access control list restricts access to the current process identity only
- **Fail-closed**: if DACL construction fails, segment/mutex creation is denied (no silent fallback)
- **Name validation**: `apc.shm_name` is validated for safe characters (alphanumeric, underscore, hyphen, dot) and length (max 200 chars)
- **ABI version guard**: `APC_SHM_ABI_VERSION` is checked first on attach — version mismatch (e.g., after DLL update) produces a fatal error directing the user to recycle the app pool
- **Serializer validation**: creator persists `apc.serializer` name in shared header; attacher validates with exact `strcmp` — mismatch is fatal (prevents silent data corruption between workers using different serializers)
- **Bounds validation**: attach path validates all shared header metadata against the actual OS-reported mapped region size via `VirtualQuery`, preventing forged metadata from inflating usable bounds
- **Integer overflow protection**: `nslots * sizeof(uintptr_t)` checked against `SIZE_MAX` before use
- **Lock safety**: lock timeout only revokes dead writers (verified via `OpenProcess` on stored PID); live slow processes are waited on indefinitely

## Process Lifecycle

```
IIS starts app pool
  -> spawns php-cgi.exe #1
    -> MINIT: acquires init mutex (DACL-protected), CreateFileMapping (NEW)
    -> initializes SMA free-list, cache header, slot table
    -> sets init_complete flag, releases init mutex
    -> serves requests, reads/writes shared cache

  -> spawns php-cgi.exe #2
    -> MINIT: acquires init mutex, CreateFileMapping (EXISTING)
    -> attaches SMA + cache (skips init, validates bounds)
    -> waits for init_complete (30s wall-clock timeout)
    -> releases init mutex
    -> sees all keys from process #1 immediately

  -> php-cgi.exe #1 crashes
    -> segment persists (process #2 still holds a handle)
    -> stale write lock detected via CAS on writer_pid, auto-recovered

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
7. **Enforced mode**: `apc.windows_shared_only = 1` with empty `apc.shm_name` = APCu disabled

## Credits

- Original APCu by [krakjoe](https://github.com/krakjoe/apcu) and contributors
- Named shared memory patch by [Lakeworks](https://github.com/lakeworks)

## License

PHP License 3.01 (same as upstream APCu)
