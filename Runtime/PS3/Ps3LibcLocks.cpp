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

#endif // POLYPHASE_PLATFORM_ADDON
