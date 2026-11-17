#include "BetaKeeper.h"

#include "AppManifestBeta.h"
#include "dllmain.h"                          // SteamInstallPath, log macros
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Manifest scanning / repair thread.
//
//  One pass reads every steamapps/appmanifest_*.acf of every library (the
//  Steam root plus everything listed in libraryfolders.vdf), remembers the
//  betakey it finds, and puts it back when it is gone. Reads are skipped for
//  files whose mtime has not moved since the previous pass, so a pass is a few
//  dozen stats when nothing is happening.
//
//  Nothing here runs on a Steam thread: NotifyBetaFallback only records an app
//  id and wakes the worker.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    using namespace SteamConfig::AppManifestBeta;

    // How often a quiet pass runs, and how fast to retry while something is
    // still owed (a fallback seen by the hook, or a write that failed because
    // Steam had the manifest open).
    constexpr std::chrono::milliseconds kIdlePoll{20000};
    constexpr std::chrono::milliseconds kBusyPoll{2000};
    constexpr std::chrono::milliseconds kSleepSlice{200};

    // The depot-config hook reports a fallback *before* steamclient rewrites the
    // manifest, so an "owed" repair is kept for a while instead of being
    // consumed by the first scan that still sees the key in place.
    constexpr std::chrono::minutes kFallbackTtl{10};

    // Manifests are ~10 KB; anything this large is not one, and must not be
    // slurped or rewritten.
    constexpr uint64_t kMaxManifestBytes = 1ull << 20;

    // Per app per session, how many repairs are worth an INFO line before
    // dropping to DEBUG (a branch Steam cannot resolve is restored on every
    // start, and the log should not fill with it).
    constexpr uint32_t kMaxRepairLogs = 3;

    std::mutex g_mutex;                                        // guards the four maps below
    std::unordered_map<AppId_t, std::string> g_pins;           // keepBeta() targets
    std::unordered_map<AppId_t, std::string> g_lastKnown;      // last non-empty betakey on disk
    std::unordered_map<AppId_t, std::string> g_selected;       // betakey currently in the manifest
    std::unordered_map<AppId_t, std::string> g_activeBranch;   // branch Steam is loading depots for
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point>
                                             g_pendingFallback; // hook saw a beta fallback
    std::unordered_map<AppId_t, uint32_t>    g_repairCount;    // repairs per app this session

    std::atomic_bool g_wake{false};
    std::atomic_bool g_started{false};
    std::atomic<int>  g_fastPasses{0};   // quick scan passes still owed

    // Caller holds g_mutex.
    void ArmFallbackLocked(AppId_t appId) {
        g_pendingFallback[appId] = std::chrono::steady_clock::now() + kFallbackTtl;
    }

    // Paths mtimes of the previous pass; worker thread only.
    using MtimeMap = std::unordered_map<std::wstring, std::filesystem::file_time_type>;

    std::filesystem::path ToPath(const std::string& utf8) {
        return std::filesystem::path(OSTPlatform::Encoding::Utf8ToWide(utf8));
    }

    // Log-safe spelling of a path: UTF-8 via the platform helper, so a library
    // on a non-ASCII path cannot throw out of a log statement.
    std::string PathForLog(const std::filesystem::path& path) {
        return OSTPlatform::Encoding::WideToUtf8(path.wstring());
    }

    std::optional<std::string> ReadSmallFile(const std::filesystem::path& path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxManifestBytes) return std::nullopt;

        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;

        std::string text(static_cast<size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (in.gcount() != static_cast<std::streamsize>(text.size())) return std::nullopt;
        return text;
    }

    // Atomic replace: write a sibling temp file, then move it over the manifest.
    // A failed move (Steam holding the file) leaves the original untouched and
    // is retried on the next pass.
    bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::path temp = path;
        temp += L".bst-tmp";

        HANDLE handle = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            LOG_BETA_WARN("cannot open temp file for {} (err={})", PathForLog(temp), ::GetLastError());
            return false;
        }

        DWORD written = 0;
        const BOOL ok = ::WriteFile(handle, text.data(),
                                    static_cast<DWORD>(text.size()), &written, nullptr);
        ::CloseHandle(handle);

        if (!ok || written != static_cast<DWORD>(text.size())) {
            LOG_BETA_WARN("write failed for {} (err={})", PathForLog(temp), ::GetLastError());
            ::DeleteFileW(temp.c_str());
            return false;
        }

        if (!::MoveFileExW(temp.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            LOG_BETA_WARN("replace failed for {} (err={}) — retrying next pass",
                          PathForLog(path), ::GetLastError());
            ::DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }

    // Every <library>/steamapps directory: the Steam root itself plus each
    // library listed in steamapps/libraryfolders.vdf.
    std::vector<std::filesystem::path> AppDirs(const std::string& steamRoot) {
        std::vector<std::filesystem::path> dirs;
        const std::filesystem::path root = ToPath(steamRoot);
        dirs.push_back(root / L"steamapps");

        if (const auto vdf = ReadSmallFile(root / L"steamapps" / L"libraryfolders.vdf")) {
            for (const std::string& folder : LibraryFolders(*vdf)) {
                const std::filesystem::path library = ToPath(folder);
                if (!library.empty()) dirs.push_back(library / L"steamapps");
            }
        }

        std::sort(dirs.begin(), dirs.end());
        dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
        return dirs;
    }

    bool IsManifestFile(const std::filesystem::path& path) {
        const std::wstring name = path.filename().wstring();
        const std::wstring_view view(name);
        if (view.rfind(L"appmanifest_", 0) != 0) return false;
        if (view.size() <= 12) return false;                        // "appmanifest_" + id
        return path.extension() == std::filesystem::path(L".acf");
    }

    // ── one manifest ────────────────────────────────────────────────────────
    // Reads `path` when it is new or its mtime moved, records its betakey with
    // the scan state, and repairs it when a branch is owed.
    struct Action {
        bool repaired = false;
        bool retry    = false;
    };

    Action HandleManifest(const std::filesystem::path& path, MtimeMap& mtimes) {
        Action action;

        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) return action;

        const std::wstring key = path.wstring();
        const auto seen = mtimes.find(key);
        if (seen != mtimes.end() && seen->second == mtime) return action;  // unchanged

        const auto text = ReadSmallFile(path);
        if (!text) return action;
        mtimes[key] = mtime;

        const auto appId = ParseAppId(*text);
        if (!appId) {
            LOG_BETA_DEBUG("{}: no appid, skipping", PathForLog(path));
            return action;
        }

        std::optional<std::string> current = FindBetaKey(*text, "UserConfig");
        if (!current) current = FindBetaKey(*text, "MountedConfig");

        // Decide what this app owes, under the lock and in one shot.
        std::string wanted;
        const char* reason = nullptr;
        bool fallbackReason = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (current) {
                g_lastKnown[*appId] = *current;
                g_selected[*appId]  = *current;
            } else {
                g_selected.erase(*appId);
            }

            const auto pin = g_pins.find(*appId);
            const bool pinned = pin != g_pins.end() && !pin->second.empty();
            if (pinned) {
                wanted = pin->second;
                reason = "keepBeta pin";
            }

            const auto pending = g_pendingFallback.find(*appId);
            if (pending != g_pendingFallback.end()) {
                if (std::chrono::steady_clock::now() > pending->second) {
                    g_pendingFallback.erase(pending);          // stale, drop it
                } else if (!current) {
                    // The manifest has already lost the key — this is the
                    // rewrite the fallback was warning about.
                    g_pendingFallback.erase(pending);
                    if (!pinned) {
                        const auto last = g_lastKnown.find(*appId);
                        if (last != g_lastKnown.end()) {
                            wanted = last->second;
                            reason = "Steam fell back to public";
                            fallbackReason = true;
                        }
                    }
                }
                // Key still present: keep the obligation armed, steamclient has
                // not persisted the fallback yet.
            }

            // The key vanished with no fallback involved. If steamclient is
            // loading manifests for a test branch right now, that is Steam
            // rewriting its app config from scratch (the branch it is running
            // never changes) — put the selection back.
            if (wanted.empty() && !current) {
                const auto active = g_activeBranch.find(*appId);
                if (active != g_activeBranch.end() && !active->second.empty()) {
                    wanted = active->second;
                    reason = "Steam is running that branch";
                }
            }
        }

        if (wanted.empty()) {
            LOG_BETA_TRACE("app {}: betakey={} (nothing owed)", *appId,
                           current ? *current : std::string("<none>"));
            return action;
        }
        if (current && *current == wanted) return action;   // already where it belongs

        const auto patched = WithBetaKey(*text, "UserConfig", wanted);
        if (!patched) return action;

        if (!WriteFileAtomic(path, *patched)) {
            // Forget the mtime so the next pass re-reads this manifest instead of
            // treating it as unchanged, and put the obligation back.
            mtimes.erase(key);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (fallbackReason) ArmFallbackLocked(*appId);
            }
            action.retry = true;
            return action;
        }

        std::string had;
        if (current) {
            had = "\"" + *current + "\"";
        } else {
            had = "absent";
        }

        uint32_t repairs = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            repairs = ++g_repairCount[*appId];
        }
        if (repairs <= kMaxRepairLogs) {
            LOG_BETA_INFO("app {}: restored UserConfig betakey \"{}\" (was {}; {})",
                          *appId, wanted, had, reason ? reason : "unknown");
        } else {
            LOG_BETA_DEBUG("app {}: restored UserConfig betakey \"{}\" ({}x this session)",
                           *appId, wanted, repairs);
        }

        action.repaired = true;
        return action;
    }

    struct ScanResult {
        bool repaired = false;
        bool retry    = false;
    };

    ScanResult Scan(const std::string& steamRoot, MtimeMap& mtimes) {
        ScanResult result;
        for (const auto& dir : AppDirs(steamRoot)) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;

            std::filesystem::directory_iterator it(dir, ec);
            const std::filesystem::directory_iterator end;
            while (!ec && it != end) {
                if (IsManifestFile(it->path())) {
                    const Action action = HandleManifest(it->path(), mtimes);
                    result.repaired = result.repaired || action.repaired;
                    result.retry    = result.retry    || action.retry;
                }
                it.increment(ec);
            }
            if (ec) {
                LOG_BETA_DEBUG("scan of {} stopped: {}", PathForLog(dir), ec.message());
            }
        }
        return result;
    }

    // True while an armed repair is still within its deadline; also drops the
    // entries that expired.
    bool HasPendingFallback() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_pendingFallback.begin(); it != g_pendingFallback.end();) {
            if (now > it->second) {
                it = g_pendingFallback.erase(it);
            } else {
                ++it;
            }
        }
        return !g_pendingFallback.empty();
    }

    void SleepUnlessWoken(std::chrono::milliseconds budget) {
        std::chrono::milliseconds waited{0};
        while (waited < budget) {
            if (g_wake.exchange(false)) return;
            std::this_thread::sleep_for(kSleepSlice);
            waited += kSleepSlice;
        }
        g_wake.store(false);
    }

    uint32_t Worker(std::string steamRoot, MtimeMap mtimes) {
        LOG_BETA_INFO("watching app manifests under {} (pins and post-fallback repairs)",
                      steamRoot);
        for (;;) {
            const ScanResult result = Scan(steamRoot, mtimes);
            const int fast = g_fastPasses.load();
            if (fast > 0) g_fastPasses.store(fast - 1);
            const bool busy = result.repaired || result.retry || fast > 0 || HasPendingFallback();
            SleepUnlessWoken(busy ? kBusyPoll : kIdlePoll);
        }
        return 0;
    }

} // namespace

namespace BetaKeeper {

    void Pin(AppId_t appId, std::string branch) {
        if (appId == k_uAppIdInvalid) return;
        if (branch.empty()) {
            Unpin(appId);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pins[appId] = branch;
        }
        LOG_BETA_INFO("app {}: pinned to branch \"{}\"", appId, branch);
        g_wake.store(true);
    }

    void Unpin(AppId_t appId) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pins.erase(appId);
        }
        LOG_BETA_INFO("app {}: branch pin cleared", appId);
    }

    std::optional<std::string> Pinned(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_pins.find(appId);
        if (it == g_pins.end()) return std::nullopt;
        return it->second;
    }

    std::optional<std::string> LastKnownBranch(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_lastKnown.find(appId);
        if (it == g_lastKnown.end()) return std::nullopt;
        return it->second;
    }

    std::optional<std::string> SelectedBranch(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_selected.find(appId);
        if (it == g_selected.end() || it->second.empty()) return std::nullopt;
        return it->second;
    }

    void NotifyBetaFallback(AppId_t appId) {
        if (appId == k_uAppIdInvalid) return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ArmFallbackLocked(appId);
        }
        g_wake.store(true);
    }

    void NoteActiveBranch(AppId_t appId, std::string_view branch) {
        if (appId == k_uAppIdInvalid || branch.empty()) return;
        // "public" (and Steam's other default spellings) is not a test branch.
        if (SteamConfig::AppManifestBeta::EqualsNoCase(branch, "public") ||
            SteamConfig::AppManifestBeta::EqualsNoCase(branch, "default")) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_activeBranch[appId] = std::string(branch);
        }
        g_wake.store(true);
    }

    void RequestScan() {
        g_fastPasses.store(6);   // ~12 s of 2 s polling, enough to cover the rewrite
        g_wake.store(true);
    }

    void Start() {
        bool expected = false;
        if (!g_started.compare_exchange_strong(expected, true)) return;

        const std::string steamRoot(SteamInstallPath);
        if (steamRoot.empty()) {
            LOG_BETA_WARN("SteamInstallPath is empty — branch repair disabled");
            g_started.store(false);
            return;
        }

        // One synchronous pass before the worker starts: the depot-config hook
        // asks SelectedBranch() right away, and Steam may already be rebuilding
        // installed apps' configs by the time this thread runs.
        MtimeMap mtimes;
        const ScanResult firstPass = Scan(steamRoot, mtimes);
        LOG_BETA_DEBUG("first pass: {} manifest(s) known, repaired={} retry={}",
                       mtimes.size(), firstPass.repaired, firstPass.retry);

        OSTPlatform::Thread::StartDetached([steamRoot, mtimes]() -> uint32_t {
            return Worker(steamRoot, mtimes);
        });
    }

} // namespace BetaKeeper
