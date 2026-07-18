/**
 * @file SystemTypes_Platform.h
 * @brief PS3 platform extension for the engine's `SystemTypes.h` fork.
 *
 * Picked up automatically by the engine when `POLYPHASE_PLATFORM_ADDON=1`
 * is defined at compile time. ActionManager generates a bridge header at
 * `<projectDir>/Generated/PolyphasePlatform_SystemTypes.h` that includes
 * this file via absolute path. Makefile_PS3 puts the Generated/ dir on the
 * include path.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// PSL1GHT primitives we typedef / embed below.
#include <ppu-types.h>
#include <sys/thread.h>
#include <lv2/mutex.h>
#include <lv2/sysfs.h>

// ----- Threading typedefs --------------------------------------------------
// PSL1GHT PPU threads are u64 ids (sys_ppu_thread_t); the thread entry is
// `void (*)(void*)` — i.e. it returns void. So ThreadFuncRet is void and we
// #define POLYPHASE_PLATFORM_ADDON_VOID_THREAD_RETURN so the engine's
// THREAD_RETURN() macro resolves to `return;`.
//
// Mutexes use PSL1GHT's lightweight-mutex struct (sys_lwmutex_t). Because it's
// a value struct (not a handle), MutexObject IS the struct and SYS_CreateMutex
// heap-allocates one; MutexObject* threads through the engine as usual.
typedef sys_ppu_thread_t ThreadObject;
typedef sys_lwmutex_t    MutexObject;
typedef void             ThreadFuncRet;
#define POLYPHASE_PLATFORM_ADDON_VOID_THREAD_RETURN

// ----- DirEntry injection --------------------------------------------------
// PS3 directory iteration uses sysFsOpendir (returns an s32 fd, NOT a POSIX
// DIR* — PSL1GHT's newlib has no <dirent.h>) and sysFsReaddir (yields a
// sysFSDirent). System_PS3.cpp stores the dir fd and last-read entry here.
#define POLYPHASE_PLATFORM_ADDON_DIRENTRY_MEMBERS \
    s32          mDirFd     = -1; \
    sysFSDirent  mLastDirent = {};

// ----- SystemState injection -----------------------------------------------
// PS3 outputs at a fixed 720p logical size for this port. We track a quit flag
// for the main loop, a background flag, the RSX context pointer the graphics
// backend brings up, and a frame counter for the flip ring.
#define POLYPHASE_PLATFORM_ADDON_SYSTEMSTATE_MEMBERS \
    bool     mQuitRequested = false; \
    bool     mInBackground  = false; \
    void*    mRsxContext    = nullptr; \
    uint32_t mFrameIndex    = 0;
