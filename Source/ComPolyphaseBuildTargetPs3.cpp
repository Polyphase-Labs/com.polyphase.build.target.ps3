// MSVC SDL deprecation suppression — every CRT call is bounds-checked.
#define _CRT_SECURE_NO_WARNINGS

/**
 * @file ComPolyphaseBuildTargetPs3.cpp
 * @brief PS3 (PSL1GHT / ps3dev) build-target addon for Polyphase Engine.
 *
 * Adds a "Sony PS3 (PSL1GHT)" entry to the editor's Build Profile dropdown.
 *
 * Toolchain expectations on the host:
 *
 *   Linux / macOS — native:
 *     - PS3DEV env var pointing at the ps3dev install prefix (canonical layout
 *       puts the PPU cross-tools under $PS3DEV/ppu/bin/ and the packaging tools
 *       under $PS3DEV/bin/). PSL1GHT points at the same dir as PS3DEV.
 *
 *   Windows — via WSL (PSL1GHT ships no native Windows binaries):
 *     - `wsl` available on PATH (Windows 10 2004+ / Windows 11).
 *     - A WSL distro with ps3dev/PSL1GHT installed inside it (default prefix
 *       /usr/local/ps3dev). The addon assumes the default distro; override with
 *       the `ps3.wslDistro` profile option to pick a specific one.
 *     - Windows-side paths are translated to /mnt/<drive>/... automatically, so
 *       the user never needs to think about the mount layout.
 *     - IMPORTANT: ps3dev's env vars ($PS3DEV/$PSL1GHT/PATH) live in ~/.bashrc,
 *       which a non-interactive `bash -lc` does NOT source on Ubuntu. Every
 *       shell body this addon emits therefore exports them itself via
 *       Ps3DevAutoDetectPrelude() (or, for compile, passes them to `make`).
 *
 *   Optional everywhere:
 *     - RPCS3 for emulator launch (PS3_EMULATOR env var overrides the binary).
 *     - ps3load for dev-console deploy (PS3LOAD=tcp://<ip> selects the target).
 *
 * Project expectations:
 *   - The PS3 build's makefile (`Makefile_PS3`) ships INSIDE this addon, at
 *     `Packages/com.polyphase.build.target.ps3/Makefile_PS3`. Projects don't
 *     have to add anything themselves; the addon's GetCompileCommand passes
 *     the addon-relative path to `make -f`. Users can override via the
 *     "Custom Makefile" build-profile option.
 *   - The ICON0.PNG XMB tile lives wherever the project keeps it (project-
 *     relative path in the profile). It is optional — a default ships with
 *     ps3dev ($PS3DEV/bin/ICON0.PNG).
 *
 * Licensing isolation: the engine binary never links against PSL1GHT. Every
 * ppu-* / PSL1GHT reference lives only in this addon (and its Runtime/PS3/
 * platform layer, which is compiled INTO the game .elf, not the editor DLL).
 *
 * Maintainer: Polyphase Engine team.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"

#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#include "imgui.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

#if EDITOR
namespace
{
    // ----- Helpers ----------------------------------------------------------

    std::string GetEnvOrEmpty(const char* name)
    {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    }

    bool FileExists(const std::string& path)
    {
        if (path.empty()) return false;
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) { std::fclose(f); return true; }
        return false;
    }

    // ----- Per-profile option keys -----------------------------------------

    constexpr const char* kTitleKey      = "ps3.title";        // shown on XMB
    constexpr const char* kAppIdKey       = "ps3.appId";        // 9-char TITLE_ID e.g. "POLY00001"
    constexpr const char* kContentIdKey   = "ps3.contentId";    // 36-char CONTENT_ID; derived from appId if empty
    constexpr const char* kIconPngKey     = "ps3.icon0";        // 320x176 ICON0.PNG (project-relative)
    constexpr const char* kMakefileKey    = "ps3.makefile";     // bare filename inside addon root, or absolute override
    constexpr const char* kJobsKey        = "ps3.jobs";         // make -j parallelism (default 4; engine TUs are 1-2 GB/TU peak)
    constexpr const char* kWslDistroKey   = "ps3.wslDistro";    // Windows only — override default WSL distro
    constexpr const char* kPs3DevPathKey  = "ps3.ps3devPath";   // path to ps3dev install inside the build shell
    constexpr const char* kRpcs3PathKey   = "ps3.rpcs3Path";    // path/name of the RPCS3 executable
    constexpr const char* kPs3LoadHostKey = "ps3.ps3loadHost";  // tcp://<ip> for ps3load device deploy

    constexpr const char* kTitleDefault   = "Polyphase Game";
    constexpr const char* kAppIdDefault    = "POLY00001";
    constexpr const char* kMakefileDefault = "Makefile_PS3";

    // Engine TUs are heavy — Engine.cpp, Renderer.cpp, Bullet, Vorbis etc.
    // each peak at 1-2 GB of ppu-gcc RAM. Plain `make -j` means unlimited
    // parallelism, which OOM-freezes 16 GB hosts within seconds. Capping at
    // 4 jobs gives ~6-8 GB peak — safe baseline. Users with 32+ GB hosts
    // can bump this via the "Jobs" build-profile option.
    constexpr const char* kJobsDefault    = "4";
    // No default for PS3DEV path — leave empty so the makefile's own fallback
    // ladder kicks in (/usr/local/ps3dev). Users with non-standard installs
    // enter their path in the Target Options panel.

    // Build a CONTENT_ID from a 9-char app/title id when the user didn't
    // supply one explicitly. Layout: <region><digits>-<APPID>_00-<16 zeros>.
    // "UP0001" is the standard homebrew/dev region+segment prefix pkg tools
    // accept; the trailing 16-char part is the package number (all zeros for
    // a single-part homebrew pkg). Total length is 36, which `pkg` requires.
    std::string DeriveContentId(const std::string& appId)
    {
        return std::string("UP0001-") + appId + "_00-0000000000000000";
    }

#if defined(_WIN32)
    // Translate a Windows absolute path into its default WSL2 mount-point:
    //   C:\Foo\Bar  ->  /mnt/c/Foo/Bar
    std::string WinToWslPath(const std::string& winPath)
    {
        if (winPath.empty()) return winPath;
        if (winPath[0] == '/') return winPath; // already POSIX
        if (winPath.size() >= 2 && winPath[1] == ':')
        {
            std::string out = "/mnt/";
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(winPath[0])));
            for (size_t i = 2; i < winPath.size(); ++i)
            {
                out += (winPath[i] == '\\') ? '/' : winPath[i];
            }
            return out;
        }
        std::string out = winPath;
        for (char& c : out) if (c == '\\') c = '/';
        return out;
    }
#endif

    // Render a host path for embedding inside a `wsl bash -lc "..."`-style
    // command. On Windows we pre-translate to /mnt/<drive>/... so the bash
    // body never sees backslashes, and single-quote it so spaces survive.
    std::string ShellPath(const std::string& path)
    {
#if defined(_WIN32)
        return std::string("'") + WinToWslPath(path) + "'";
#else
        return std::string("'") + path + "'";
#endif
    }

    // Emit "wsl [-d <distro>]" prefix on Windows; empty on POSIX.
    std::string WslPrefix(const PolyphaseBuildContext* ctx)
    {
#if defined(_WIN32)
        std::string distro;
        if (ctx != nullptr && ctx->GetProfileSetting != nullptr)
        {
            char buf[128] = {0};
            if (ctx->GetProfileSetting(kWslDistroKey, buf, sizeof(buf)) != 0 && buf[0] != '\0')
            {
                distro = buf;
            }
        }
        if (distro.empty()) return std::string("wsl ");
        return std::string("wsl -d ") + distro + " ";
#else
        (void)ctx;
        return std::string();
#endif
    }

    // Wrap a single-line bash command for the host:
    //   Windows: `wsl [-d distro] bash -lc "<body>"`
    //   POSIX:   `bash -lc "<body>"`
    // The body must use SINGLE quotes around its paths/strings — the outer
    // double quotes survive cmd.exe/CreateProcess as one argument, while inner
    // single quotes are inert to both.
    std::string WrapShell(const PolyphaseBuildContext* ctx, const std::string& body)
    {
        return WslPrefix(ctx) + "bash -lc \"" + body + "\"";
    }

    // Returns a shell prelude that exports ps3dev's env explicitly, then
    // auto-detects the install prefix from the same fallback ladder Makefile_PS3
    // uses. Needed because `bash -lc` from the editor's WSL dispatch doesn't
    // source ~/.bashrc on Ubuntu (the stock .bashrc early-returns for non-
    // interactive shells), so ps3dev's exports there are invisible.
    //
    // Used for the PostPackage / RunOnDevice tool invocations, which run inside
    // bash directly (not through make). Composes cleanly with `&&` into a body.
    std::string Ps3DevAutoDetectPrelude()
    {
        return
            "for d in /usr/local/ps3dev /opt/ps3dev \\\"\\$HOME/ps3dev\\\" "
            "/mnt/c/ps3dev /mnt/d/ps3dev; do "
            "if [ -x \\\"\\$d/ppu/bin/ppu-gcc\\\" ]; then "
            "export PS3DEV=\\\"\\$d\\\"; break; fi; done && "
            "export PSL1GHT=\\\"\\$PS3DEV\\\" && "
            "export PATH=\\\"\\$PS3DEV/bin:\\$PS3DEV/ppu/bin:\\$PS3DEV/spu/bin:\\$PATH\\\"";
    }

    std::string ReadOption(const PolyphaseBuildContext* ctx, const char* key, const char* fallback)
    {
        if (ctx == nullptr || ctx->GetProfileSetting == nullptr) return fallback ? fallback : "";
        char buf[512] = {0};
        if (ctx->GetProfileSetting(key, buf, sizeof(buf)) == 0 || buf[0] == '\0')
        {
            return fallback ? fallback : "";
        }
        return std::string(buf);
    }

    // Resolve the app/content id pair, applying defaults and deriving the
    // content id from the app id when the user left it blank.
    void ResolveIds(const PolyphaseBuildContext* ctx, std::string& appId, std::string& contentId)
    {
        appId = ReadOption(ctx, kAppIdKey, kAppIdDefault);
        contentId = ReadOption(ctx, kContentIdKey, "");
        if (contentId.empty()) contentId = DeriveContentId(appId);
    }

    // ----- Build-target callbacks ------------------------------------------

    int32_t Ps3_Validate(char* outReason, size_t cap)
    {
#if defined(_WIN32)
        // Windows hosts run PSL1GHT inside WSL — ps3dev ships no native Windows
        // binaries. Probe by running the auto-detect prelude and checking that
        // both the compiler and the NPDRM packager are reachable inside WSL.
        char probe[1024];
        std::snprintf(probe, sizeof(probe),
            "wsl bash -lc \"for d in /usr/local/ps3dev /opt/ps3dev \\\"$HOME/ps3dev\\\" "
            "/mnt/c/ps3dev; do if [ -x \\\"$d/ppu/bin/ppu-gcc\\\" ]; then "
            "export PS3DEV=\\\"$d\\\"; export PATH=\\\"$d/bin:$d/ppu/bin:$PATH\\\"; break; fi; done; "
            "command -v ppu-gcc >/dev/null 2>&1 && command -v make_self_npdrm >/dev/null 2>&1\"");
        const int rc = std::system(probe);
        if (rc != 0)
        {
            std::snprintf(outReason, cap,
                "PS3 toolchain not reachable via WSL. Install WSL2 + Ubuntu, then inside "
                "WSL install ps3dev/PSL1GHT (https://github.com/ps3dev/ps3toolchain) to "
                "/usr/local/ps3dev. Set the Target Options 'WSL Distro' field to pick a "
                "specific distro if you have more than one.");
            return 0;
        }
        return 1;
#else
        std::string ps3dev = GetEnvOrEmpty("PS3DEV");
        if (ps3dev.empty()) ps3dev = "/usr/local/ps3dev";
        if (!FileExists(ps3dev + "/ppu/bin/ppu-gcc"))
        {
            std::snprintf(outReason, cap,
                "PS3DEV='%s' but ppu-gcc not found under ppu/bin/. Install ps3dev/PSL1GHT "
                "(https://github.com/ps3dev/ps3toolchain) or set PS3DEV to the install prefix.",
                ps3dev.c_str());
            return 0;
        }
        if (!FileExists(ps3dev + "/bin/make_self_npdrm"))
        {
            std::snprintf(outReason, cap,
                "PS3DEV='%s' but make_self_npdrm not found under bin/. Re-run the ps3toolchain "
                "install (it builds the PSL1GHT packaging tools).", ps3dev.c_str());
            return 0;
        }
        return 1;
#endif
    }

    int32_t Ps3_GetCompileCommand(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr) return 0;

        const std::string makefileOpt = ReadOption(ctx, kMakefileKey,   kMakefileDefault);
        const std::string ps3devPath  = ReadOption(ctx, kPs3DevPathKey, "");
        const std::string jobsOpt     = ReadOption(ctx, kJobsKey,       kJobsDefault);

        // Validate jobs — must be a positive decimal int. Anything else falls
        // back to default so we never emit `make -j abc` or `make -j-99`.
        int jobs = 0;
        for (char c : jobsOpt) { if (c < '0' || c > '9') { jobs = 0; break; } jobs = jobs * 10 + (c - '0'); }
        if (jobs < 1 || jobs > 64) jobs = 4;

        // Resolve the makefile path. Makefile_PS3 ships inside the addon — bare
        // filenames (the default) live under
        // <projectDir>/Packages/com.polyphase.build.target.ps3/. Absolute
        // overrides (starting with '/' or a Windows drive letter) are taken
        // as-is so users can point at a fork.
        const bool isAbsolute =
            !makefileOpt.empty() &&
            (makefileOpt[0] == '/' ||
             (makefileOpt.size() >= 2 && makefileOpt[1] == ':'));
        const std::string makefilePath = isAbsolute
            ? makefileOpt
            : (std::string(ctx->projectDir) +
               "/Packages/com.polyphase.build.target.ps3/" + makefileOpt);

        // Pass PS3DEV directly to make so the Makefile doesn't depend on the
        // shell having sourced ~/.bashrc (Ubuntu's stock .bashrc early-returns
        // for non-interactive shells). Plain VAR=VAL on the make line takes
        // precedence over any assignments inside the makefile body.
        std::string makePs3Dev;
        if (!ps3devPath.empty())
        {
            makePs3Dev = " PS3DEV='" + ps3devPath + "' PSL1GHT='" + ps3devPath + "'";
        }

        // Pass POLYPHASE_PATH from the build context so the Makefile never has
        // to guess the engine root from a walk-up.
        std::string makePolyphasePath;
        if (ctx->engineDir != nullptr && ctx->engineDir[0] != '\0')
        {
            makePolyphasePath = " POLYPHASE_PATH=" + ShellPath(ctx->engineDir);
        }

        char jobsArg[16];
        std::snprintf(jobsArg, sizeof(jobsArg), " -j%d", jobs);

        // Out-of-tree build: .o / .d intermediates live in
        // <projectDir>/Intermediate/PS3/. Make's CWD = that dir; the Makefile's
        // PROJECT_ROOT var lets it find source/include/output paths relative to
        // the user's actual project. Keeps the project root clean.
        const std::string intermediateDir =
            std::string(ctx->projectDir) + "/Intermediate/PS3";
        const std::string mkIntermediate =
            "mkdir -p " + ShellPath(intermediateDir) + " && ";

        // Force Rebuild: clean intermediates AND the staged binaries in
        // Build/PS3.
        std::string cleanPrefix;
        if (ctx->forceRebuild)
        {
            cleanPrefix =
                "(cd " + ShellPath(intermediateDir) +
                " && rm -f *.o *.d *.elf *.self 2>/dev/null; true) && " +
                "(rm -f " + ShellPath(std::string(ctx->projectDir) + "/Build/PS3") +
                "/*.elf " + ShellPath(std::string(ctx->projectDir) + "/Build/PS3") +
                "/*.self 2>/dev/null; true) && ";
        }

        const std::string makeProjectRoot =
            " PROJECT_ROOT=" + ShellPath(ctx->projectDir);

        const std::string body =
            mkIntermediate +
            cleanPrefix +
            "make -C " + ShellPath(intermediateDir) +
            " -f " + ShellPath(makefilePath) +
            makeProjectRoot + makePs3Dev + makePolyphasePath + jobsArg;

        std::snprintf(outCmd, cap, "%s", WrapShell(ctx, body).c_str());
        return 1;
    }

    int32_t Ps3_GetCompiledBinaryPath(const PolyphaseBuildContext* ctx, char* outPath, size_t cap)
    {
        if (ctx == nullptr || ctx->projectDir == nullptr || ctx->projectName == nullptr) return 0;

        // Makefile_PS3 links a .elf then fake-signs it into a directly-bootable
        // .self (via fself), staging BOTH to Build/PS3/. The .self is the
        // primary artifact the engine copies into the package and RunInEmulator
        // hands to RPCS3. PostPackage reaches back to the staged .elf (via
        // ctx->projectDir) to build the installable .pkg.
        std::snprintf(outPath, cap, "%s/Build/PS3/%s.self",
                      ctx->projectDir, ctx->projectName);
        return 1;
    }

    int32_t Ps3_PostPackage(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr ||
            ctx->projectName == nullptr || ctx->projectDir == nullptr) return 0;

        const std::string title = ReadOption(ctx, kTitleKey, kTitleDefault);
        std::string appId, contentId;
        ResolveIds(ctx, appId, contentId);

        // Resolve the optional ICON0.PNG (project-relative) to absolute. Falls
        // back to the ps3dev-bundled default when the project ships none.
        std::string iconPath;
        if (ctx->ResolvePath != nullptr)
        {
            const std::string rel = ReadOption(ctx, kIconPngKey, "");
            if (!rel.empty())
            {
                char abs[1024] = {0};
                if (ctx->ResolvePath(rel.c_str(), abs, sizeof(abs))) iconPath = abs;
            }
        }

        const std::string outDir  = ctx->packageOutputDir;
        // The staged .elf (produced alongside the .self by Makefile_PS3's
        // stage: rule) is what make_self_npdrm signs into EBOOT.BIN.
        const std::string elfPath = std::string(ctx->projectDir) + "/Build/PS3/" +
                                    ctx->projectName + ".elf";
        const std::string pkgDir  = outDir + "/pkg";
        const std::string usrDir  = pkgDir + "/USRDIR";
        const std::string pkgPath = outDir + "/" + ctx->projectName + ".pkg";
        const std::string finalPkg = outDir + "/" + ctx->projectName + ".gnpdrm.pkg";

        // icon0: user asset if present, else $PS3DEV/bin/ICON0.PNG (resolved in
        // the shell so we don't hard-code the prefix). Both branches copy into
        // pkg/ICON0.PNG.
        std::string icon0Copy;
        if (!iconPath.empty())
        {
            icon0Copy = "cp " + ShellPath(iconPath) + " " + ShellPath(pkgDir + "/ICON0.PNG");
        }
        else
        {
            icon0Copy = "cp \\\"\\$PS3DEV/bin/ICON0.PNG\\\" " + ShellPath(pkgDir + "/ICON0.PNG");
        }

        // Title may contain spaces / quotes — escape single quotes safely.
        auto quoteForBash = [](const std::string& s) {
            std::string out = "'";
            for (char c : s)
            {
                if (c == '\'') out += "'\\''";
                else           out += c;
            }
            out += '\'';
            return out;
        };
        const std::string titleQuoted = quoteForBash(title);

        // One bash body assembles the whole pkg tree and finalizes it:
        //   pkg/
        //     ICON0.PNG
        //     PARAM.SFO
        //     USRDIR/EBOOT.BIN   (the NPDRM-signed .elf)
        //   -> <name>.pkg -> <name>.gnpdrm.pkg (finalized/re-signed)
        std::string body;
        body += Ps3DevAutoDetectPrelude() + " && ";
        body += "rm -rf " + ShellPath(pkgDir) + " && ";
        body += "mkdir -p " + ShellPath(usrDir) + " && ";
        body += icon0Copy + " && ";
        body += "make_self_npdrm " + ShellPath(elfPath) + " " +
                ShellPath(usrDir + "/EBOOT.BIN") + " " + contentId + " && ";
        body += "sfo --title " + titleQuoted + " --appid '" + appId + "' "
                "-f \\\"\\$PS3DEV/bin/sfo.xml\\\" " + ShellPath(pkgDir + "/PARAM.SFO") + " && ";
        body += "pkg --contentid " + contentId + " " +
                ShellPath(pkgDir + "/") + " " + ShellPath(pkgPath) + " && ";
        body += "cp " + ShellPath(pkgPath) + " " + ShellPath(finalPkg) + " && ";
        body += "package_finalize " + ShellPath(finalPkg);

        const std::string cmd = WrapShell(ctx, body);
        if (ctx->WriteOutputLine != nullptr) ctx->WriteOutputLine(cmd.c_str());
        const int rc = std::system(cmd.c_str());
        if (rc != 0)
        {
            if (ctx->Log != nullptr)
            {
                char msg[512];
                std::snprintf(msg, sizeof(msg),
                    "PS3 pkg build failed (rc=%d). Verify make_self_npdrm / sfo / pkg / "
                    "package_finalize are reachable (WSL on Windows; $PS3DEV/bin on POSIX) "
                    "and that Build/PS3/%s.elf exists.", rc, ctx->projectName);
                ctx->Log(POLYPHASE_BT_LOG_ERROR, msg);
            }
            return 0;
        }

        if (ctx->Log != nullptr)
        {
            char ok[512];
            std::snprintf(ok, sizeof(ok),
                "PS3 package complete: %s (CONTENT_ID=%s). Bootable .self is alongside it; "
                "install %s on a real console or RPCS3.",
                pkgPath.c_str(), contentId.c_str(), finalPkg.c_str());
            ctx->Log(POLYPHASE_BT_LOG_DEBUG, ok);
        }
        return 1;
    }

    int32_t Ps3_RunInEmulator(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        // RPCS3 boots a fake-signed .self directly. Users override the binary
        // via PS3_EMULATOR (env) or the ps3.rpcs3Path profile option.
        std::string exe = GetEnvOrEmpty("PS3_EMULATOR");
        if (exe.empty()) exe = ReadOption(ctx, kRpcs3PathKey, "");
        if (exe.empty()) exe = "rpcs3.exe";

        std::snprintf(outCmd, cap, "\"%s\" \"%s/%s.self\"",
                      exe.c_str(), ctx->packageOutputDir, ctx->projectName);
        return 1;
    }

    int32_t Ps3_RunOnDevice(const PolyphaseBuildContext* ctx, char* outCmd, size_t cap)
    {
        // ps3load uploads and runs a .self on a dev console listening for the
        // ps3load protocol. It reads the target from the PS3LOAD env var
        // (tcp://<ip>). Users set it via env or the ps3.ps3loadHost option.
        if (ctx == nullptr || ctx->packageOutputDir == nullptr || ctx->projectName == nullptr) return 0;

        std::string host = GetEnvOrEmpty("PS3LOAD");
        if (host.empty()) host = ReadOption(ctx, kPs3LoadHostKey, "");
        if (host.empty())
        {
            std::snprintf(outCmd, cap,
                "echo \"PS3LOAD not set. Boot the console into a ps3load listener, then set "
                "PS3LOAD=tcp://<ip> (env) or the 'ps3load host' Target Option.\" && exit 1");
            return 1;
        }

        const std::string self = std::string(ctx->packageOutputDir) + "/" + ctx->projectName + ".self";
        const std::string body = Ps3DevAutoDetectPrelude() +
                                 " && export PS3LOAD=" + host +
                                 " && ps3load " + ShellPath(self);
        std::snprintf(outCmd, cap, "%s", WrapShell(ctx, body).c_str());
        return 1;
    }

    void Ps3_DrawProfileOptions(const PolyphaseBuildContext* ctx)
    {
        if (ctx == nullptr || ctx->SetProfileSetting == nullptr) return;

        auto textField = [&](const char* label, const char* key, const char* def,
                             size_t bufSize, const char* tooltip) {
            std::string current = ReadOption(ctx, key, def);
            std::string buf(bufSize, '\0');
            std::strncpy(&buf[0], current.c_str(), bufSize - 1);
            if (ImGui::InputText(label, &buf[0], bufSize))
            {
                ctx->SetProfileSetting(key, buf.c_str());
            }
            if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        };

        textField("Title (XMB)", kTitleKey, kTitleDefault, 128,
                  "Shown in the XMB / on the game tile.");
        textField("App ID (TITLE_ID)", kAppIdKey, kAppIdDefault, 16,
                  "9-character TITLE_ID (e.g. POLY00001). Used for the CONTENT_ID and save namespace.");
        textField("Content ID", kContentIdKey, "", 48,
                  "36-char CONTENT_ID (e.g. UP0001-POLY00001_00-0000000000000000).\n"
                  "Leave empty to derive it from the App ID automatically.");
        textField("ICON0.PNG (320x176)", kIconPngKey, "", 256,
                  "Project-relative path to the game tile thumbnail (PNG, 320x176).\n"
                  "Example: Assets/PS3/ICON0.PNG. Optional — a ps3dev default is used if empty.");
        textField("Makefile", kMakefileKey, kMakefileDefault, 256,
                  "Makefile that drives the PSL1GHT build. Bare filename resolves inside the addon "
                  "(default: Makefile_PS3). Absolute paths point at a fork.");

        // ----- make -j parallelism ----------------------------------------
        {
            std::string current = ReadOption(ctx, kJobsKey, kJobsDefault);
            int jobs = std::atoi(current.c_str());
            if (jobs < 1 || jobs > 64) jobs = 4;
            if (ImGui::SliderInt("Parallel Jobs", &jobs, 1, 32))
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d", jobs);
                ctx->SetProfileSetting(kJobsKey, buf);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("`make -j<N>` parallelism. Each ppu-gcc compile job peaks at 1-2 GB RAM.\n"
                                  "Rule of thumb: jobs = min(CPU cores, host RAM GB / 2). Default 4.");
        }

        textField("WSL Distro (Windows)", kWslDistroKey, "", 64,
                  "Windows-only: pass to `wsl -d <name>`. Leave empty to use the default distro.\n"
                  "Ignored on Linux/macOS.");
        textField("PS3DEV path", kPs3DevPathKey, "", 256,
                  "Absolute path to your ps3dev install AS SEEN BY THE BUILD SHELL.\n"
                  "  - Windows: WSL-side path, e.g. /usr/local/ps3dev or /mnt/c/ps3dev.\n"
                  "  - Linux/macOS: native path, e.g. /usr/local/ps3dev.\n"
                  "Leave empty to use the makefile's /usr/local/ps3dev default.");
        textField("RPCS3 binary", kRpcs3PathKey, "", 256,
                  "Path/name of the RPCS3 executable for 'Run in Emulator'.\n"
                  "Default: rpcs3.exe (on PATH). Override with the PS3_EMULATOR env var too.");
        textField("ps3load host", kPs3LoadHostKey, "", 64,
                  "tcp://<ip> of a dev console running a ps3load listener, for 'Run on Device'.\n"
                  "Can also be set via the PS3LOAD env var.");

        ImGui::Spacing();
#if defined(_WIN32)
        ImGui::TextDisabled("Windows: PSL1GHT runs inside WSL. Install WSL2 + Ubuntu, then");
        ImGui::TextDisabled("install ps3dev to /usr/local/ps3dev (github.com/ps3dev/ps3toolchain).");
#else
        ImGui::TextDisabled("Requires ps3dev/PSL1GHT ($PS3DEV) with ppu-gcc + the packaging tools on PATH.");
#endif
        ImGui::TextDisabled("Default emulator is rpcs3.exe — override with PS3_EMULATOR env var.");
        ImGui::TextDisabled("Set PS3LOAD=tcp://<ip> to enable 'Run on Device' via ps3load.");
    }

    // Canonical descriptor. Strings are deep-copied by the registry; this
    // static instance just needs to outlive the RegisterBuildTarget call.
    static PolyphaseBuildTargetDesc gPs3Target{};
}
#endif // EDITOR

// ----- Plugin lifecycle -----------------------------------------------------

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    if (api) api->LogDebug("com.polyphase.build.target.ps3 loaded.");
    return 0;
}

static void OnUnload()
{
    if (sEngineAPI) sEngineAPI->LogDebug("com.polyphase.build.target.ps3 unloaded.");
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* /*nodeFactory*/) {}
static void RegisterScriptFuncs(lua_State* L) { (void)L; }

#if EDITOR
static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    if (hooks == nullptr) return;

    if (hooks->RegisterBuildTarget == nullptr)
    {
        if (sEngineAPI)
        {
            sEngineAPI->LogWarning("com.polyphase.build.target.ps3: this engine "
                                   "build predates the build-target API (need plugin "
                                   "apiVersion >= 4). Target not registered.");
        }
        return;
    }

    gPs3Target = {};
    gPs3Target.apiVersion            = POLYPHASE_BUILD_TARGET_API_VERSION;
    gPs3Target.targetId              = "homebrew.ps3";
    gPs3Target.displayName           = "Sony PS3 (PSL1GHT)";
    gPs3Target.iconText              = "";
    gPs3Target.category              = "Retro Consoles";
    gPs3Target.basePlatform          = 1; /* Platform::Linux — Unix-like cook + ELF */
    gPs3Target.binaryExtension       = ".self";
    gPs3Target.requiresDocker        = 0;
    gPs3Target.supportsRunOnDevice   = 1;
    gPs3Target.supportsEmulator      = 1;
    gPs3Target.Validate              = &Ps3_Validate;
    gPs3Target.PreCook               = nullptr;
    gPs3Target.CookAsset             = nullptr; // Linux cook is fine for PS3 V1
    gPs3Target.GetCompileCommand     = &Ps3_GetCompileCommand;
    gPs3Target.GetCompiledBinaryPath = &Ps3_GetCompiledBinaryPath;
    gPs3Target.PostPackage           = &Ps3_PostPackage;
    gPs3Target.RunOnDevice           = &Ps3_RunOnDevice;
    gPs3Target.RunInEmulator         = &Ps3_RunInEmulator;
    gPs3Target.DrawProfileOptions    = &Ps3_DrawProfileOptions;
    gPs3Target.SerializeProfileOptions   = nullptr;
    gPs3Target.DeserializeProfileOptions = nullptr;

    // Variant 2: point the engine at the addon-shipped platform extension
    // headers. Interpreted relative to the addon root (the dir containing
    // package.json). When this target is selected, ActionManager writes
    // Generated/PolyphasePlatform_*.h bridges that #include the addon's
    // SystemTypes_Platform.h / InputTypes_Platform.h / AudioTypes_Platform.h /
    // NetworkTypes_Platform.h by absolute path. Makefile_PS3 then sets
    // -DPOLYPHASE_PLATFORM_ADDON=1 and -I<Generated/> so the engine's fork
    // headers pick up the addon-provided typedefs.
    gPs3Target.platformExtensionDir = "Runtime/PS3";

    hooks->RegisterBuildTarget(hookId, &gPs3Target);
}
#endif

extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    if (desc == nullptr) return 1;
    desc->apiVersion          = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName          = "com.polyphase.build.target.ps3";
    desc->pluginVersion       = "1.0.0";
    desc->OnLoad              = OnLoad;
    desc->OnUnload            = OnUnload;
    desc->Tick                = nullptr;
    desc->TickEditor          = nullptr;
    desc->RegisterTypes       = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI    = RegisterEditorUI;
#else
    desc->RegisterEditorUI    = nullptr;
#endif
    desc->OnEditorPreInit     = nullptr;
    desc->OnEditorReady       = nullptr;
    return 0;
}
