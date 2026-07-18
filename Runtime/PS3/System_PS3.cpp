/**
 * @file System_PS3.cpp
 * @brief PS3-side implementation of the engine's SYS_* surface (PSL1GHT).
 *
 * Mirrors the shape of System_PSP.cpp — a fixed-output console with a Unix-ish
 * SDK — but uses PSL1GHT / lv2 primitives:
 *
 *   - File + directory I/O via lv2 sysFs* (PSL1GHT's newlib has no POSIX
 *     <dirent.h>; paths are /dev_hdd0/... , /dev_bdvd/... etc).
 *   - Threads are sys_ppu_thread_t via sysThreadCreate.
 *   - "Mutexes" are lightweight mutexes (sys_lwmutex_t).
 *   - Time is sysGetSystemTime (µs).
 *   - No window concepts (720p fixed); window/clipboard/dialog SYS_ are
 *     no-op stubs.
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined. Symbols that
 * SystemUtils.cpp already provides for addon builds (SYS_ExecFull,
 * SYS_FileOpenRead/Read/Seek/Close, SYS_DrainDroppedFiles, ...) are NOT
 * redefined here — this file matches System_PSP.cpp's exact surface.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "System/System.h"
#include "Engine.h"
#include "Stream.h"
#include "Log.h"
#include "Utilities.h"

#include <ppu-types.h>
#include <lv2/sysfs.h>
#include <sys/thread.h>
#include <lv2/mutex.h>
#include <sys/systime.h>
#include <sys/tty.h>

#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>

// lv2 sysFs open flags. PSL1GHT names them SYS_O_*.
#ifndef SYS_O_RDONLY
#define SYS_O_RDONLY 0x0000
#endif
#ifndef SYS_O_WRONLY
#define SYS_O_WRONLY 0x0001
#endif
#ifndef SYS_O_CREAT
#define SYS_O_CREAT  0x0200
#endif
#ifndef SYS_O_TRUNC
#define SYS_O_TRUNC  0x0400
#endif
#ifndef SYS_O_APPEND
#define SYS_O_APPEND 0x0008
#endif

// Directory-entry type marker returned by sysFsReaddir. lv2 uses 1 for a
// directory and 2 for a regular file (CELL_FS_TYPE_DIRECTORY / _REGULAR).
#ifndef SYS_FS_TYPE_DIRECTORY
#define SYS_FS_TYPE_DIRECTORY 1
#endif

// Default asset root under a packaged pkg install (PS3_GAME/USRDIR maps to
// /dev_hdd0/game/<TITLEID>/USRDIR/ at runtime). For a bare-.self run under
// RPCS3 assets aren't required to reach the first cleared frame.
#ifndef POLYPHASE_PS3_GAME_DIR
#define POLYPHASE_PS3_GAME_DIR "POLYPHASE"
#endif

static bool sInitialized = false;

// =========================================================================
// Lifecycle
// =========================================================================

void SYS_Initialize()
{
    if (sInitialized) return;
    sInitialized = true;
    LogDebug("System_PS3: initialised");
}

void SYS_Shutdown()
{
    // RSX/video teardown lives in Graphics_PS3RSX::GFX_Shutdown; nothing else
    // to release here.
    sInitialized = false;
}

void SYS_Update()
{
    // No callback pump needed on PS3 (the XMB/Home overlay is handled by the
    // sysutil callback queue, which a full port would drain here via
    // sysUtilCheckCallback). Phase-1 boot doesn't register any.
}

// =========================================================================
// Paths
// =========================================================================

std::string SYS_GetExecutablePath()
{
    return "/dev_hdd0/game/" POLYPHASE_PS3_GAME_DIR "/USRDIR/EBOOT.BIN";
}

std::string SYS_GetPolyphasePath()
{
    // Asset root. A packaged pkg lays assets under USRDIR alongside EBOOT.BIN.
    static std::string sBase;
    if (sBase.empty())
    {
        sysFSStat st;
        if (sysFsStat("/dev_bdvd/PS3_GAME/USRDIR", &st) == 0)
            sBase = "/dev_bdvd/PS3_GAME/USRDIR/";
        else
            sBase = "/dev_hdd0/game/" POLYPHASE_PS3_GAME_DIR "/USRDIR/";
    }
    return sBase;
}

std::string SYS_GetCurrentDirectoryPath()
{
    return SYS_GetPolyphasePath();
}

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    // Paths on PS3 are already absolute (/dev_hdd0/, /dev_bdvd/, /dev_usb/).
    if (!relativePath.empty() && relativePath[0] == '/') return relativePath;
    return SYS_GetPolyphasePath() + relativePath;
}

void SYS_ExplorerOpenDirectory(const std::string& /*dirPath*/) {}
void SYS_OpenFileWithDefaultApp(const std::string& /*filePath*/) {}
void SYS_SetWorkingDirectory(const std::string& /*dirPath*/) {}

// =========================================================================
// File I/O
// =========================================================================

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
    sysFSStat st;
    return sysFsStat(path, &st) == 0;
}

void SYS_AcquireFileData(const char* path, bool /*isAsset*/, int32_t maxSize,
                         char*& outData, uint32_t& outSize)
{
    outData = nullptr;
    outSize = 0;
    if (path == nullptr) return;

    s32 fd = -1;
    if (sysFsOpen(path, SYS_O_RDONLY, &fd, NULL, 0) != 0 || fd < 0)
    {
        LogWarning("SYS_AcquireFileData: sysFsOpen failed for '%s'", path);
        return;
    }

    u64 endPos = 0;
    sysFsLseek(fd, 0, SEEK_END, &endPos);
    u64 zero = 0;
    sysFsLseek(fd, 0, SEEK_SET, &zero);

    uint32_t actualSize = (uint32_t)endPos;
    if (maxSize > 0 && actualSize > (uint32_t)maxSize) actualSize = (uint32_t)maxSize;

    outData = (char*)malloc(actualSize);
    if (outData == nullptr)
    {
        sysFsClose(fd);
        LogError("SYS_AcquireFileData: malloc(%u) failed for '%s'", actualSize, path);
        return;
    }

    u64 nread = 0;
    const s32 rc = sysFsRead(fd, outData, actualSize, &nread);
    sysFsClose(fd);

    if (rc != 0)
    {
        free(outData);
        outData = nullptr;
        LogError("SYS_AcquireFileData: sysFsRead failed for '%s'", path);
        return;
    }
    outSize = (uint32_t)nread;
}

void SYS_ReleaseFileData(char* data)
{
    free(data);
}

bool SYS_CreateDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return false;
    return sysFsMkdir(dirPath, 0777) == 0;
}

void SYS_RemoveDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return;
    sysFsRmdir(dirPath);
}

void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    outDirEntry.mValid = false;
    if (sysFsOpendir(dirPath.c_str(), &outDirEntry.mDirFd) != 0 || outDirEntry.mDirFd < 0)
    {
        outDirEntry.mDirFd = -1;
        return;
    }

    strncpy(outDirEntry.mDirectoryPath, dirPath.c_str(), MAX_PATH_SIZE);
    outDirEntry.mDirectoryPath[MAX_PATH_SIZE] = '\0';
    outDirEntry.mValid = true;

    // Prime the first entry so callers get a stable invariant after Open.
    SYS_IterateDirectory(outDirEntry);
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    if (!dirEntry.mValid || dirEntry.mDirFd < 0)
    {
        dirEntry.mValid = false;
        return;
    }

    u64 nread = 0;
    const s32 rc = sysFsReaddir(dirEntry.mDirFd, &dirEntry.mLastDirent, &nread);
    if (rc != 0 || nread == 0)
    {
        // Error or end-of-directory.
        dirEntry.mValid = false;
        return;
    }

    strncpy(dirEntry.mFilename, dirEntry.mLastDirent.d_name, MAX_PATH_SIZE);
    dirEntry.mFilename[MAX_PATH_SIZE] = '\0';
    dirEntry.mDirectory = (dirEntry.mLastDirent.d_type == SYS_FS_TYPE_DIRECTORY);
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    if (dirEntry.mDirFd >= 0)
    {
        sysFsClosedir(dirEntry.mDirFd);
        dirEntry.mDirFd = -1;
    }
    dirEntry.mValid = false;
}

bool SYS_CopyFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return false;

    s32 src = -1;
    if (sysFsOpen(sourcePath, SYS_O_RDONLY, &src, NULL, 0) != 0 || src < 0) return false;

    s32 dst = -1;
    if (sysFsOpen(destPath, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &dst, NULL, 0) != 0 || dst < 0)
    {
        sysFsClose(src);
        return false;
    }

    bool copyOk = true;
    char buf[8192];
    for (;;)
    {
        u64 nread = 0;
        if (sysFsRead(src, buf, sizeof(buf), &nread) != 0) { copyOk = false; break; }
        if (nread == 0) break;
        u64 nwritten = 0;
        if (sysFsWrite(dst, buf, nread, &nwritten) != 0 || nwritten != nread) { copyOk = false; break; }
    }

    sysFsClose(src);
    sysFsClose(dst);
    return copyOk;
}

void SYS_CopyDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
bool SYS_CopyDirectoryRecursive(const std::string& /*sourceDir*/, const std::string& /*destDir*/) { return false; }
// PSL1GHT's lv2 sysFs has no rename syscall — move/rename fall back to
// copy+unlink for files and a no-op for directories. These aren't on the
// boot path (they're used by editor/packaging tooling, not the runtime).
void SYS_MoveDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
void SYS_MoveFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return;
    if (SYS_CopyFile(sourcePath, destPath)) sysFsUnlink(sourcePath);
}
void SYS_RemoveFile(const char* path)
{
    if (path) sysFsUnlink(path);
}
bool SYS_Rename(const char* oldPath, const char* newPath)
{
    if (oldPath == nullptr || newPath == nullptr) return false;
    if (!SYS_CopyFile(oldPath, newPath)) return false;
    sysFsUnlink(oldPath);
    return true;
}

std::vector<std::string> SYS_OpenFileDialog() { return {}; }
std::string SYS_SaveFileDialog() { return ""; }
std::string SYS_SelectFolderDialog() { return ""; }

std::string SYS_GetFileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// =========================================================================
// Threading
// =========================================================================

namespace
{
    // PSL1GHT thread entry is `void (*)(void*)`. The engine's ThreadFuncFP is
    // also void(*)(void*) (ThreadFuncRet == void on PS3), so we can trampoline
    // directly, ending with sysThreadExit for a clean termination + join.
    struct Ps3ThreadShim
    {
        ThreadFuncFP mFunc;
        void*        mArg;
    };

    void Ps3ThreadEntry(void* argp)
    {
        Ps3ThreadShim* shim = static_cast<Ps3ThreadShim*>(argp);
        if (shim != nullptr && shim->mFunc != nullptr)
        {
            shim->mFunc(shim->mArg);
        }
        delete shim;
        sysThreadExit(0);
    }
}

ThreadObject* SYS_CreateThread(ThreadFuncFP func, void* arg)
{
    if (func == nullptr) return nullptr;

    Ps3ThreadShim* shim = new Ps3ThreadShim{ func, arg };

    ThreadObject* out = new ThreadObject;
    // Priority 1000 (mid), 64 KB stack. flags=0 → joinable so SYS_JoinThread
    // works. Engine worker threads (asset loader, audio) don't need more.
    const s32 rc = sysThreadCreate(out, &Ps3ThreadEntry, shim, 1000, 0x10000, 0,
                                   (char*)"polyphase_thread");
    if (rc != 0)
    {
        LogError("SYS_CreateThread: sysThreadCreate failed (rc=%d)", (int)rc);
        delete shim;
        delete out;
        return nullptr;
    }
    return out;
}

void SYS_JoinThread(ThreadObject* thread)
{
    if (thread == nullptr) return;
    u64 retval = 0;
    sysThreadJoin(*thread, &retval);
}

void SYS_DestroyThread(ThreadObject* thread)
{
    // The thread frees its own shim and calls sysThreadExit; the id itself
    // needs no explicit destroy once joined. Just release our handle.
    delete thread;
}

MutexObject* SYS_CreateMutex()
{
    MutexObject* out = new MutexObject;
    sys_lwmutex_attr_t attr;
    attr.attr_protocol  = SYS_LWMUTEX_PROTOCOL_PRIO;
    attr.attr_recursive = SYS_LWMUTEX_ATTR_NOT_RECURSIVE;
    strncpy(attr.name, "polymtx", sizeof(attr.name));
    if (sysLwMutexCreate(out, &attr) != 0)
    {
        LogError("SYS_CreateMutex: sysLwMutexCreate failed");
        delete out;
        return nullptr;
    }
    return out;
}

void SYS_LockMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    sysLwMutexLock(mutex, 0);   // 0 = no timeout (block)
}

void SYS_UnlockMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    sysLwMutexUnlock(mutex);
}

void SYS_DestroyMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    sysLwMutexDestroy(mutex);
    delete mutex;
}

void SYS_Sleep(uint32_t milliseconds)
{
    sysUsleep(milliseconds * 1000);
}

// =========================================================================
// Time
// =========================================================================

uint64_t SYS_GetTimeMicroseconds()
{
    return (uint64_t)sysGetSystemTime();
}

// =========================================================================
// Process exec — not applicable on PS3.
// =========================================================================

void SYS_Exec(const char* /*cmd*/, std::string* output)
{
    if (output) output->clear();
}

// =========================================================================
// Memory
// =========================================================================

void* SYS_AlignedMalloc(uint32_t size, uint32_t alignment)
{
    return memalign(alignment, size);
}

void SYS_AlignedFree(void* pointer)
{
    free(pointer);
}

std::vector<MemoryStat> SYS_GetMemoryStats()
{
    std::vector<MemoryStat> stats;

    // PS3 game partition: ~213 MB main RAM available to a title. Phase-1 has
    // no per-pool allocator tracking, so report the ceiling with 0 used.
    MemoryStat mainRam;
    mainRam.mName = "MainRAM";
    mainRam.mBytesFree = 213u * 1024u * 1024u;
    mainRam.mBytesAllocated = 0;
    stats.push_back(mainRam);

    MemoryStat vram;
    vram.mName = "VRAM";
    vram.mBytesFree = 256u * 1024u * 1024u; // RSX local memory
    vram.mBytesAllocated = 0;
    stats.push_back(vram);

    return stats;
}

float SYS_GetRAMUsage()   { return 0.0f; }
float SYS_GetVRAMUsage()  { return 0.0f; }
float SYS_GetRAM1Usage()  { return 0.0f; }
float SYS_GetRAM2Usage()  { return 0.0f; }
float SYS_GetCPUUsage()   { return 0.0f; }
float SYS_GetTotalRAM()   { return (float)(213u * 1024u * 1024u); }
float SYS_GetTotalVRAM()  { return (float)(256u * 1024u * 1024u); }
float SYS_GetTotalRAM1()  { return SYS_GetTotalRAM(); }
float SYS_GetTotalRAM2()  { return 0.0f; }

// =========================================================================
// Save data — flat files under /dev_hdd0/game/<GAMEID>/USRDIR/savedata/.
// Uses the engine's Stream file I/O, which routes through SYS_AcquireFileData
// above (native sysFs) for reads. Writes go through Stream::WriteFile.
// =========================================================================

namespace
{
    inline std::string SaveDir()
    {
        return "/dev_hdd0/game/" POLYPHASE_PS3_GAME_DIR "/USRDIR/savedata/";
    }

    inline std::string SavePath(const char* saveName)
    {
        return SaveDir() + saveName;
    }

    inline void EnsureSaveDirExists()
    {
        sysFsMkdir("/dev_hdd0/game/" POLYPHASE_PS3_GAME_DIR, 0777);
        sysFsMkdir("/dev_hdd0/game/" POLYPHASE_PS3_GAME_DIR "/USRDIR", 0777);
        sysFsMkdir("/dev_hdd0/game/" POLYPHASE_PS3_GAME_DIR "/USRDIR/savedata", 0777);
    }
}

bool SYS_ReadSave(const char* saveName, Stream& outStream)
{
    if (saveName == nullptr) return false;
    if (!SYS_DoesSaveExist(saveName)) return false;
    const std::string path = SavePath(saveName);
    outStream.ReadFile(path.c_str(), /*isAsset=*/false);
    return outStream.GetSize() > 0;
}

bool SYS_WriteSave(const char* saveName, Stream& stream)
{
    if (saveName == nullptr) return false;
    EnsureSaveDirExists();
    const std::string path = SavePath(saveName);
    return stream.WriteFile(path.c_str());
}

bool SYS_DoesSaveExist(const char* saveName)
{
    if (saveName == nullptr) return false;
    return SYS_DoesFileExist(SavePath(saveName).c_str(), false);
}

bool SYS_DeleteSave(const char* saveName)
{
    if (saveName == nullptr) return false;
    return sysFsUnlink(SavePath(saveName).c_str()) == 0;
}

void SYS_UnmountMemoryCard() {}

// =========================================================================
// Clipboard — N/A
// =========================================================================

void SYS_SetClipboardText(const std::string& /*str*/) {}
std::string SYS_GetClipboardText() { return ""; }

// =========================================================================
// Logging / assertions / dialogs
// =========================================================================

// HDD root (not /dev_hdd0/tmp, which RPCS3's virtual HDD doesn't have by
// default — sysFsOpen(O_CREAT) won't create the parent dir). Shared with
// Main_PS3's boot log so all output lands in one file.
static const char* sLogFilePath = "/dev_hdd0/polyphase_ps3.log";

static void Ps3_AppendLogLineRaw(const char* line)
{
    if (line == nullptr) return;

    // Output via the lv2 TTY syscall (RPCS3 shows this in its TTY window) plus
    // stdout. We do NOT write a log file: /dev_hdd0 root is not writable under
    // RPCS3 (cellFsOpen -> CELL_EACCES) and /dev_hdd0/tmp doesn't exist, so a
    // per-line file open just spams errors. TTY is the reliable channel.
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    u32 w = 0;
    sysTtyWrite(0, line, (u32)strlen(line), &w);
    sysTtyWrite(0, "\n", 1, &w);
}

void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, arg);

    const char* sevTag = (severity == LogSeverity::Error)   ? "[E] "
                       : (severity == LogSeverity::Warning) ? "[W] "
                       :                                       "[D] ";

    char line[1100];
    snprintf(line, sizeof(line), "%s%s", sevTag, buf);
    Ps3_AppendLogLineRaw(line);
}

void SYS_Assert(const char* exprString, const char* fileString, uint32_t lineNumber)
{
    char line[1100];
    snprintf(line, sizeof(line), "ASSERT: %s at %s:%u",
                  exprString, fileString, (unsigned)lineNumber);
    Ps3_AppendLogLineRaw(line);
}

void SYS_Alert(const char* message)
{
    char line[1100];
    snprintf(line, sizeof(line), "ALERT: %s", message ? message : "");
    Ps3_AppendLogLineRaw(line);
}

void SYS_UpdateConsole() {}

int32_t SYS_GetPlatformTier()
{
    return 0;
}

// =========================================================================
// Window — all no-ops on PS3 (fixed 720p output).
// =========================================================================

void SYS_SetWindowTitle(const char* /*title*/) {}
void SYS_SetWindowIcon(const char* /*iconPath*/) {}
bool SYS_DoesWindowHaveFocus() { return true; }
void SYS_SetScreenOrientation(ScreenOrientation /*orientation*/) {}
ScreenOrientation SYS_GetScreenOrientation() { return ScreenOrientation::Landscape; }
void SYS_SetFullscreen(bool /*fullscreen*/) {}
bool SYS_IsFullscreen() { return true; }
void SYS_SetWindowRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
void SYS_GetWindowRect(int32_t& outX, int32_t& outY, int32_t& outWidth, int32_t& outHeight)
{
    outX = 0;
    outY = 0;
    outWidth  = 1280;
    outHeight = 720;
}
bool SYS_IsWindowMaximized() { return true; }
void SYS_MaximizeWindow() {}

#endif // POLYPHASE_PLATFORM_ADDON
