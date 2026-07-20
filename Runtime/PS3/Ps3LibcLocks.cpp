/**
 * @file Ps3LibcLocks.cpp
 * @brief Override PSL1GHT newlib's libc locks with single-threaded no-ops.
 *
 * Root cause of the pre-main abort on PS3/RPCS3:
 *   libstdc++'s locale/facet initialization (cxx11-shim_facets.cc, pulled in by
 *   any TU that touches <iostream>/<sstream>/locale) runs inside a GLOBAL
 *   CONSTRUCTOR — before main(). It takes a newlib recursive libc lock, which on
 *   PSL1GHT is backed by an lv2 sys_lwmutex that has NOT been created that early
 *   in startup. `_sys_lwmutex_lock` then returns CELL_ESRCH (invalid id) and
 *   newlib calls abort() → the process dies before main with no other trace.
 *   (This is exactly the "_sys_lwmutex_lock failed ESRCH" line seen in the RPCS3
 *   log on every boot.)
 *
 * Fix: replace the lv2-lwmutex-backed libc lock primitives with no-ops. These
 * are strong definitions; the addon links with `-Wl,--allow-multiple-definition`
 * so object-file symbols win over the libc.a copies. libc internal locking then
 * does nothing, so the too-early locale init no longer touches an uninitialised
 * lwmutex and the process reaches main() and boots.
 *
 * Caveat: this makes libc's *internal* locking single-threaded (malloc, stdio
 * FILE pool, locale). Fine for the current boot / cleared-screen milestone (the
 * hot path is effectively single-threaded). Before relying on concurrent libc
 * access from multiple engine threads, replace these with real mutual exclusion
 * (e.g. GCC __atomic spinlocks, which don't depend on the lv2 lock subsystem).
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include <sys/lock.h>   // __libc_lock_t / __libc_lock_recursive_t (== struct _sys_lwmutex)

extern "C" {

// All return int (0 = success) and take a pointer to the lock struct.
int __libc_lock_init(__libc_lock_t* /*lock*/)                     { return 0; }
int __libc_lock_init_recursive(__libc_lock_recursive_t* /*lock*/) { return 0; }
int __libc_lock_close(__libc_lock_t* /*lock*/)                    { return 0; }
int __libc_lock_close_recursive(__libc_lock_recursive_t* /*lock*/){ return 0; }
int __libc_lock_acquire(__libc_lock_t* /*lock*/)                     { return 0; }
int __libc_lock_acquire_recursive(__libc_lock_recursive_t* /*lock*/) { return 0; }
int __libc_lock_try_acquire(__libc_lock_t* /*lock*/)                     { return 0; }
int __libc_lock_try_acquire_recursive(__libc_lock_recursive_t* /*lock*/) { return 0; }
int __libc_lock_release(__libc_lock_t* /*lock*/)                     { return 0; }
int __libc_lock_release_recursive(__libc_lock_recursive_t* /*lock*/) { return 0; }

// stdio FILE-pool lock (newlib __sfp_lock_*), no arguments.
void __sfp_lock_acquire(void) {}
void __sfp_lock_release(void) {}

}

// -------------------------------------------------------------------------
// Zero-initializing malloc (console-port UB mitigation).
//
// The engine has latent uninitialized-variable reads that are benign on the
// desktop — where fresh OS/heap pages read back as zero, so an uninitialized
// pointer is null and the engine's `if (ptr)` guards skip the bad path. Under
// RPCS3, freshly-allocated memory is POISONED with 0xABADCAFE, so those same
// reads yield a non-null garbage pointer that passes the guards and is then
// dereferenced / memcpy'd → access violation during scene load (nodes,
// components, Datums, properties). Zero every allocation so PS3 sees the same
// zeroed memory the desktop does. Linked via `-Wl,--wrap=malloc` (see
// Makefile_PS3); operator new/new[] route through malloc, so this covers them
// too. calloc already zeroes; realloc's grown tail is left as-is (rare path).
#include <stdlib.h>
#include <string.h>

extern "C" void* __real_malloc(size_t size);
extern "C" void* __wrap_malloc(size_t size)
{
    void* p = __real_malloc(size);
    if (p != nullptr) memset(p, 0, size);
    return p;
}

// -------------------------------------------------------------------------
// Safe fstat/stat (avoid the packed sysFSStat syscall crash).
//
// PSL1GHT's newlib fstat/stat fill a `__attribute__((packed))` sysFSStat that
// lands unaligned on the stack; under RPCS3 the lv2 stat syscall's write into
// it faults on a SUCCESSFUL stat (a missing path errors before the write, which
// is why reads via sysFsOpen never tripped it — see SYS_DoesFileExist). newlib
// stdio calls fstat internally on fopen() to pick a buffering mode, so
// Stream::WriteFile("wb") — used by the save system — crashes the moment it
// opens a real file. Override the syscall stubs to report a plain regular file
// (full buffering) without touching sysFsFstat; size is not needed by stdio's
// buffering decision. Wins over libc.a via -Wl,--allow-multiple-definition.
#include <sys/stat.h>
#include <sys/reent.h>

static int Ps3FillRegularStat(struct stat* st)
{
    if (st == nullptr) return -1;
    memset(st, 0, sizeof(struct stat));
    st->st_mode    = S_IFREG | 0666;
    st->st_blksize = 4096;
    st->st_nlink   = 1;
    return 0;
}

extern "C" int _fstat_r(struct _reent* /*r*/, int /*fd*/, struct stat* st) { return Ps3FillRegularStat(st); }
extern "C" int fstat(int /*fd*/, struct stat* st)                          { return Ps3FillRegularStat(st); }
extern "C" int _fstat(int /*fd*/, struct stat* st)                         { return Ps3FillRegularStat(st); }

#endif // POLYPHASE_PLATFORM_ADDON
