#include "BetaKeeper.h"

#include "AppInfoFile.h"
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

    // How long after an announced fallback a depot-set change is treated as part
    // of that fallback (Steam rewrites the set back to the public build right
    // after it) and therefore must not be captured as the branch's own build.
    constexpr std::chrono::minutes kFallbackCaptureGuard{15};

    // A repair must not fight the user: when Steam writes a different branch
    // shortly after we restored one (a branch switch in the UI looks exactly like
    // a loss — the key is briefly absent), stand down for a while instead of
    // putting the old value back.
    constexpr std::chrono::minutes kStandDown{5};
    constexpr std::chrono::minutes kStandDownWindow{5};

    // A branch switch in the Betas dialog looks like a loss for a moment: the
    // client rewrites the manifest with "UserConfig" but no betakey first, then
    // writes the new branch and starts the update. Restoring during that window
    // cancels the switch (measured: click at 11:17:06, client wrote the keyless
    // manifest at 11:17:09, our restore of the old branch at 11:17:11 aborted it).
    // So wait this long before treating an empty key as a lost selection.
    constexpr std::chrono::seconds kRestoreGrace{12};

    std::mutex g_mutex;                                        // guards the four maps below
    std::unordered_map<AppId_t, std::string> g_pins;           // keepBeta() targets
    std::unordered_map<AppId_t, std::string> g_lastKnown;      // last non-empty betakey on disk
    std::unordered_map<AppId_t, std::string> g_selected;       // betakey currently in the manifest
    std::unordered_map<AppId_t, std::string> g_activeBranch;   // branch Steam is loading depots for
    std::unordered_map<AppId_t, std::vector<InstalledDepot>> g_installed; // installed build per app
    // "appid|branch" -> depot set of that branch's build, persisted across launches.
    std::unordered_map<std::string, std::vector<InstalledDepot>> g_branchDepots;
    std::mutex g_cacheMutex;                                   // serialises cache file writes
    std::atomic_bool g_cacheDirty{false};
    // When steamclient last announced a beta fallback per app. A fallback is
    // followed by a rewrite of the depot set back to the public build, so any
    // depot change near it must not be mistaken for the branch's own build.
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_fallbackAt;
    std::unordered_map<AppId_t, std::string> g_restoredValue;   // what we last wrote
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_restoreAt;
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_standDownUntil;
    std::unordered_map<AppId_t, std::chrono::steady_clock::time_point> g_missingSince;
    // Byte offset into <Steam>/logs/content_log.txt; the client logs every branch
    // the user picks there, which is the only place a *requested* branch shows up
    // before the client has written it to the manifest.
    std::uintmax_t g_contentLogOffset = 0;
    bool g_contentLogStarted = false;
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

    // Defined further down; the branch depot cache helpers use them.
    std::string PathForLog(const std::filesystem::path& path);
    std::optional<std::string> ReadSmallFile(const std::filesystem::path& path);
    bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text);

    std::filesystem::path ToPath(const std::string& utf8) {
        return std::filesystem::path(OSTPlatform::Encoding::Utf8ToWide(utf8));
    }

    // <Steam>/opensteamtool/beta-depots.txt — depot sets remembered per branch.
    std::filesystem::path BranchCachePath(const std::string& steamRoot) {
        return ToPath(steamRoot) / L"opensteamtool" / L"beta-depots.txt";
    }

    // Line format: "<appid>\t<branch>\t<depotId>:<gid>:<size>;<depotId>:<gid>:<size>"
    void LoadBranchCache(const std::string& steamRoot) {
        const auto text = ReadSmallFile(BranchCachePath(steamRoot));
        if (!text) return;

        size_t entries = 0;
        size_t pos = 0;
        while (pos < text->size()) {
            size_t end = text->find('\n', pos);
            if (end == std::string::npos) end = text->size();
            std::string_view line(text->data() + pos, end - pos);
            pos = end + 1;
            if (line.empty() || line.front() == '#') continue;

            const size_t tab = line.find('\t');
            if (tab == std::string_view::npos) continue;

            // "<appid>|<branch>" then a tab, then the depot list.
            const std::string_view head = line.substr(0, tab);
            const std::string_view depots = line.substr(tab + 1);
            const size_t bar = head.find('|');
            if (bar == std::string_view::npos) continue;

            const std::string appIdText(head.substr(0, bar));
            const std::string branch(head.substr(bar + 1));
            if (appIdText.empty() || branch.empty()) continue;

            AppId_t appId = 0;
            for (char ch : appIdText) {
                if (ch < '0' || ch > '9') { appId = 0; break; }
                appId = appId * 10 + static_cast<AppId_t>(ch - '0');
            }
            if (!appId) continue;

            std::vector<InstalledDepot> set;
            size_t p = 0;
            while (p < depots.size()) {
                size_t semi = depots.find(';', p);
                if (semi == std::string_view::npos) semi = depots.size();
                const std::string_view item = depots.substr(p, semi - p);
                p = semi + 1;

                const size_t c1 = item.find(':');
                const size_t c2 = c1 == std::string_view::npos ? std::string_view::npos : item.find(':', c1 + 1);
                if (c2 == std::string_view::npos) continue;

                InstalledDepot depot;
                depot.depotId = static_cast<AppId_t>(detail::ParseDigits(item.substr(0, c1)));
                depot.gid     = detail::ParseDigits(item.substr(c1 + 1, c2 - c1 - 1));
                depot.size    = detail::ParseDigits(item.substr(c2 + 1));
                if (depot.depotId && depot.gid) set.push_back(depot);
            }
            if (set.empty()) continue;

            std::lock_guard<std::mutex> lock(g_mutex);
            g_branchDepots[std::to_string(appId) + "|" + branch] = std::move(set);
            ++entries;
        }
        LOG_BETA_INFO("branch depot cache: {} entr(ies) loaded", entries);
        if (entries == 0 && !text->empty()) {
            const size_t sample = text->size() < 120 ? text->size() : 120;
            LOG_BETA_DEBUG("branch depot cache: nothing parsed from \"{}\"",
                           std::string(text->data(), sample));
        }
    }

    void SaveBranchCache(const std::string& steamRoot) {
        std::string out;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto& [key, set] : g_branchDepots) {
                out += key;
                out += '\t';
                for (size_t i = 0; i < set.size(); ++i) {
                    if (i) out += ';';
                    out += std::to_string(set[i].depotId);
                    out += ':';
                    out += std::to_string(set[i].gid);
                    out += ':';
                    out += std::to_string(set[i].size);
                }
                out += '\n';
            }
        }

        std::lock_guard<std::mutex> fileLock(g_cacheMutex);
        std::error_code ec;
        const auto path = BranchCachePath(steamRoot);
        std::filesystem::create_directories(path.parent_path(), ec);
        if (!WriteFileAtomic(path, out)) {
            LOG_BETA_WARN("could not write {}", PathForLog(path));
        }
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

    std::optional<AppId_t> AppIdFromPath(const std::filesystem::path& path) {
        const std::wstring name = path.stem().wstring();   // "appmanifest_3450310"
        if (name.rfind(L"appmanifest_", 0) != 0) return std::nullopt;
        AppId_t id = 0;
        bool any = false;
        for (size_t i = 12; i < name.size(); ++i) {
            const wchar_t ch = name[i];
            if (ch < L'0' || ch > L'9') return std::nullopt;
            id = id * 10 + static_cast<AppId_t>(ch - L'0');
            any = true;
        }
        if (!any || !id) return std::nullopt;
        return id;
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
        if (seen != mtimes.end() && seen->second == mtime) {
            // The client writes the keyless manifest once and then goes quiet, so
            // an unchanged file can still owe a repair: re-check it once its grace
            // window has elapsed, otherwise the timer would never be looked at.
            bool recheck = false;
            if (const auto appId = AppIdFromPath(path)) {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (const auto missing = g_missingSince.find(*appId); missing != g_missingSince.end()) {
                    recheck = (std::chrono::steady_clock::now() - missing->second) >= kRestoreGrace;
                }
            }
            if (!recheck) return action;
        }

        const auto text = ReadSmallFile(path);
        if (!text) return action;
        mtimes[key] = mtime;

        const auto appId = ParseAppId(*text);
        if (!appId) {
            LOG_BETA_DEBUG("{}: no appid, skipping", PathForLog(path));
            return action;
        }

        // "UserConfig" holds the user's selection — the record whose loss makes a
        // branch revert to default. "MountedConfig" only mirrors what the
        // installed build came from, and steamclient keeps writing THAT one even
        // while it drops "UserConfig" (measured: right after a beta fallback the
        // manifest has MountedConfig.betakey set and UserConfig.betakey gone), so
        // the two must not be conflated when deciding whether anything is owed.
        const std::optional<std::string> userKey    = FindBetaKey(*text, "UserConfig");
        const std::optional<std::string> mountedKey = FindBetaKey(*text, "MountedConfig");
        const std::optional<std::string> current    = userKey ? userKey : mountedKey;

        // Decide what this app owes, under the lock and in one shot.
        std::string wanted;
        const char* reason = nullptr;
        bool fallbackReason = false;
        const auto installedDepots = FindInstalledDepots(*text);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto now = std::chrono::steady_clock::now();

            // If the manifest now carries a branch we did not write, Steam (or the
            // user, through the Betas dialog) chose it. Do not put our value back:
            // stand down for a while so a switch is never cancelled.
            if (userKey) {
                const auto restored = g_restoredValue.find(*appId);
                if (restored != g_restoredValue.end() && *userKey != restored->second) {
                    const auto at = g_restoreAt.find(*appId);
                    if (at != g_restoreAt.end() && now - at->second < kStandDownWindow) {
                        g_standDownUntil[*appId] = now + kStandDown;
                        LOG_BETA_INFO("app {}: Steam set branch \"{}\" (we had restored \"{}\") — "
                                      "standing down", *appId, *userKey, restored->second);
                    }
                    g_restoredValue.erase(restored);
                    g_restoreAt.erase(*appId);
                }
            }

            // Did Steam change the installed depot set since the previous pass?
            bool depotsChanged = false;
            const auto prevInstalled = g_installed.find(*appId);
            if (prevInstalled != g_installed.end()) {
                const auto& prev = prevInstalled->second;
                depotsChanged = prev.size() != installedDepots.size();
                if (!depotsChanged) {
                    for (size_t i = 0; i < prev.size(); ++i) {
                        if (prev[i].depotId != installedDepots[i].depotId ||
                            prev[i].gid != installedDepots[i].gid) {
                            depotsChanged = true;
                            break;
                        }
                    }
                }
            }

            auto& installedSlot = g_installed[*appId];
            installedSlot.clear();
            for (const auto& depot : installedDepots) {
                installedSlot.push_back({depot.depotId, depot.gid, depot.size});
            }

            if (userKey) {
                g_lastKnown[*appId] = *userKey;
                g_selected[*appId]  = *userKey;
            } else {
                g_selected.erase(*appId);
            }

            // The grace window keys on UserConfig.betakey only: MountedConfig
            // keeps its own BetaKey while a switch is in flight, so testing the
            // merged value would keep the window open forever.
            if (userKey) {
                g_missingSince.erase(*appId);
            } else if (g_missingSince.find(*appId) == g_missingSince.end()) {
                g_missingSince[*appId] = now;
            }

            // Inside the grace window a missing key may simply be a switch in
            // progress; only a pin acts immediately.
            bool graceOver = false;
            if (auto missing = g_missingSince.find(*appId); missing != g_missingSince.end()) {
                graceOver = (now - missing->second) >= kRestoreGrace;
            }

            // Capture the branch's build at the only moment it is unambiguous:
            // Steam changed the installed depot set while a betakey was in the
            // manifest, and no fallback was announced for this app around that
            // change (a fallback rewrites the set back to the public build, and
            // that must never be filed as the branch's own build).
            const auto recentFallback = g_fallbackAt.find(*appId);
            const bool fallbackNearby =
                recentFallback != g_fallbackAt.end() &&
                std::chrono::steady_clock::now() - recentFallback->second < kFallbackCaptureGuard;
            if (current && depotsChanged && !installedDepots.empty() && !fallbackNearby) {
                const std::string cacheKey = std::to_string(*appId) + "|" + *current;
                std::vector<InstalledDepot>& slot = g_branchDepots[cacheKey];
                slot.clear();
                slot.reserve(installedDepots.size());
                for (const auto& depot : installedDepots) {
                    slot.push_back({depot.depotId, depot.gid, depot.size});
                }
                g_cacheDirty.store(true);
                LOG_BETA_INFO("app {}: captured branch \"{}\" build ({} depot(s))",
                              *appId, *current, slot.size());
            }

            const auto pin = g_pins.find(*appId);
            const bool pinned = pin != g_pins.end() && !pin->second.empty();
            if (pinned) {
                wanted = pin->second;
                reason = "keepBeta pin";
            }

            const auto pending = g_pendingFallback.find(*appId);
            if (!pinned && !graceOver) {
                // Waiting for a possible switch to finish: do not touch the file.
                wanted.clear();
                reason = nullptr;
            } else if (pending != g_pendingFallback.end()) {
                if (std::chrono::steady_clock::now() > pending->second) {
                    g_pendingFallback.erase(pending);          // stale, drop it
                } else if (!userKey) {
                    // steamclient has already rewritten UserConfig away (this is
                    // the fallback it announced). MountedConfig may still carry
                    // the branch, which is the best evidence of the selection.
                    g_pendingFallback.erase(pending);
                    if (!pinned) {
                        // The branch Steam is loading right now beats any value we
                        // remember: during a UI switch our last-known value is the
                        // branch being left, and restoring it would cancel the
                        // switch the user just made.
                        const auto active = g_activeBranch.find(*appId);
                        const auto last = g_lastKnown.find(*appId);
                        if (active != g_activeBranch.end() && !active->second.empty()) {
                            wanted = active->second;
                            reason = "Steam fell back to public (active branch)";
                            fallbackReason = true;
                        } else if (last != g_lastKnown.end()) {
                            wanted = last->second;
                            reason = "Steam fell back to public";
                            fallbackReason = true;
                        } else if (mountedKey && !mountedKey->empty()) {
                            wanted = *mountedKey;
                            reason = "Steam fell back to public (mounted config)";
                            fallbackReason = true;
                        }
                    }
                }
                // UserConfig still present: keep the obligation armed,
                // steamclient has not persisted the fallback yet.
            }

            // No fallback flagged, but steamclient is loading manifests for a
            // test branch right now while UserConfig has lost the key — it is
            // rewriting its app config from scratch. Put the selection back, but
            // only once the grace window has passed: inside it the missing key is
            // most likely a switch the user just started.
            if (wanted.empty() && !userKey && (graceOver || pinned)) {
                const auto active = g_activeBranch.find(*appId);
                if (active != g_activeBranch.end() && !active->second.empty()) {
                    wanted = active->second;
                    reason = "Steam is running that branch";
                }
            }

            // Plain "keep the selection": the manifest lost the key and no other
            // signal named a branch. Put the last known one back (the grace window
            // has already given a switch the chance to write its own value).
            if (wanted.empty() && !userKey && (graceOver || pinned)) {
                const auto last = g_lastKnown.find(*appId);
                if (last != g_lastKnown.end() && !last->second.empty()) {
                    wanted = last->second;
                    reason = "kept the selected branch";
                } else if (mountedKey && !mountedKey->empty()) {
                    wanted = *mountedKey;
                    reason = "kept the mounted branch";
                }
            }

            // Standing down after Steam overrode a restore: leave the file alone
            // until the window expires (a pin always wins).
            if (!wanted.empty() && !pinned) {
                const auto down = g_standDownUntil.find(*appId);
                if (down != g_standDownUntil.end()) {
                    if (now < down->second) {
                        LOG_BETA_TRACE("app {}: standing down, not restoring {}", *appId, wanted);
                        wanted.clear();
                        reason = nullptr;
                    } else {
                        g_standDownUntil.erase(down);
                    }
                }
            }
        }

        if (wanted.empty()) {
            LOG_BETA_TRACE("app {}: betakey={} (nothing owed)", *appId,
                           current ? *current : std::string("<none>"));
            return action;
        }
        if (userKey && *userKey == wanted) return action;   // already where it belongs

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

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_restoredValue[*appId] = wanted;
            g_restoreAt[*appId] = std::chrono::steady_clock::now();
        }

        std::string had;
        if (userKey) {
            had = "\"" + *userKey + "\"";
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

    // Follows Steam's own content log for "AppID <id> beta changed: <branch>" and
    // adopts it as the requested branch. Measured: a click in the Betas dialog
    // logs that line but the client writes the manifest without the key and does
    // not start any content work; the branch is applied on a later start, when
    // the manifest carries it. So recording the request here is what makes the
    // click survive to that start.
    void WatchSteamBetaChanges(const std::string& steamRoot) {
        const std::filesystem::path path = ToPath(steamRoot) / L"logs" / L"content_log.txt";
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0) return;

        if (!g_contentLogStarted) {
            // Start at the end: history from previous sessions is not a request.
            g_contentLogOffset = size;
            g_contentLogStarted = true;
            return;
        }
        if (size <= g_contentLogOffset) {
            if (size < g_contentLogOffset) g_contentLogOffset = size;   // truncated
            return;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        in.seekg(static_cast<std::streamoff>(g_contentLogOffset));
        std::string chunk;
        std::string line;
        while (std::getline(in, line)) {
            const std::string needle = " beta changed: ";
            const size_t at = line.find("AppID ");
            if (at == std::string::npos) continue;
            const size_t branchAt = line.find(needle);
            if (branchAt == std::string::npos) continue;
            // "AppID 3450310 beta changed: 1.4.2-prev_open_beta"
            const size_t idStart = at + 6;
            const size_t idEnd = line.find(' ', idStart);
            if (idEnd == std::string::npos || idEnd > branchAt) continue;
            AppId_t appId = 0;
            bool digits = idEnd > idStart;
            for (size_t i = idStart; i < idEnd; ++i) {
                const char ch = line[i];
                if (ch < '0' || ch > '9') { digits = false; break; }
                appId = appId * 10 + static_cast<AppId_t>(ch - '0');
            }
            if (!digits || !appId) continue;
            std::string branch = line.substr(branchAt + needle.size());
            while (!branch.empty() && (branch.back() == '\r' || branch.back() == ' ')) branch.pop_back();
            if (branch.empty()) continue;

            std::lock_guard<std::mutex> lock(g_mutex);
            g_lastKnown[appId] = branch;
            g_selected[appId]  = branch;
            g_missingSince.erase(appId);
            g_pendingFallback.erase(appId);
            g_standDownUntil.erase(appId);
            LOG_BETA_INFO("app {}: Steam reports branch \"{}\" — recording it as the request",
                          appId, branch);
        }
        g_contentLogOffset = size;
        g_wake.store(true);
    }

    ScanResult Scan(const std::string& steamRoot, MtimeMap& mtimes) {
        ScanResult result;
        WatchSteamBetaChanges(steamRoot);

        // Keep the branch tables current: the answer to a beta fallback is the
        // branch's own depot list, which only the client's appinfo cache has.
        AppInfoFile::Refresh(steamRoot);

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
            if (g_cacheDirty.exchange(false)) SaveBranchCache(steamRoot);
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

    std::vector<InstalledDepot> InstalledDepots(AppId_t appId) {
        std::vector<InstalledDepot> out;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_installed.find(appId);
        if (it == g_installed.end()) return out;
        out.reserve(it->second.size());
        for (const auto& depot : it->second) {
            out.push_back({depot.depotId, depot.gid, depot.size});
        }
        return out;
    }

    std::vector<InstalledDepot> BranchDepots(AppId_t appId, std::string_view branch) {
        std::vector<InstalledDepot> out;
        if (appId == k_uAppIdInvalid || branch.empty()) return out;

        // Authoritative source first: the client's own appinfo cache, which lists
        // exactly the depots and gids that branch's build is made of.
        for (const auto& depot : AppInfoFile::Lookup(appId, branch)) {
            out.push_back({depot.depotId, depot.gid, depot.size});
        }
        if (!out.empty()) return out;

        // Fall back to what an earlier launch recorded (appinfo may be missing or
        // unreadable this session).
        const std::string key = std::to_string(appId) + "|" + std::string(branch);
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_branchDepots.find(key);
        if (it == g_branchDepots.end()) return out;
        out.reserve(it->second.size());
        for (const auto& depot : it->second) {
            out.push_back({depot.depotId, depot.gid, depot.size});
        }
        return out;
    }

    uint64_t BranchBuildId(AppId_t appId, std::string_view branch) {
        if (appId == k_uAppIdInvalid || branch.empty()) return 0;
        return AppInfoFile::LookupBuildId(appId, branch);
    }

    void NoteBranchManifest(AppId_t appId, std::string_view branch, AppId_t depotId,
                            uint64_t gid, uint64_t size) {
        if (appId == k_uAppIdInvalid || depotId == 0 || gid == 0 || branch.empty()) return;
        if (EqualsNoCase(branch, "public")) return;   // that build needs no help

        const std::string key = std::to_string(appId) + "|" + std::string(branch);
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& set = g_branchDepots[key];
        for (auto& depot : set) {
            if (depot.depotId != depotId) continue;
            if (depot.gid == gid && depot.size == size) return;
            if (depot.gid != gid) {
                LOG_BETA_DEBUG("app {} branch \"{}\": depot {} gid {} -> {}", appId, branch,
                               depotId, depot.gid, gid);
                depot.gid = gid;
            }
            if (size) depot.size = size;
            g_cacheDirty.store(true);
            return;
        }
        set.push_back({depotId, gid, size});
        g_cacheDirty.store(true);
        LOG_BETA_DEBUG("app {} branch \"{}\": depot {} gid {} size {} remembered ({} depot(s))",
                       appId, branch, depotId, gid, size, set.size());
    }

    bool IsTracked(AppId_t appId) {
        if (appId == k_uAppIdInvalid) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_installed.count(appId) != 0 || g_selected.count(appId) != 0 ||
               g_lastKnown.count(appId) != 0 || g_pins.count(appId) != 0;
    }

    void NotifyBetaFallback(AppId_t appId) {
        if (appId == k_uAppIdInvalid) return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ArmFallbackLocked(appId);
            g_fallbackAt[appId] = std::chrono::steady_clock::now();
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
        LoadBranchCache(steamRoot);
        MtimeMap mtimes;
        const ScanResult firstPass = Scan(steamRoot, mtimes);
        LOG_BETA_DEBUG("first pass: {} manifest(s) known, repaired={} retry={}",
                       mtimes.size(), firstPass.repaired, firstPass.retry);

        OSTPlatform::Thread::StartDetached([steamRoot, mtimes]() -> uint32_t {
            return Worker(steamRoot, mtimes);
        });
    }

} // namespace BetaKeeper
