#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "dllmain.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/SteamConfig/BetaKeeper.h"
#include "Utils/SteamMetadata/ManifestCache.h"
#include "OSTPlatform/include/Dialog.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

    // Per-fetch timeout for the on-demand pre-seed in YldLoadDepotManifest. Most
    // manifests are Cloudflare edge hits (~100-300 ms); this cap keeps one slow or
    // missing manifest from stalling Steam for long, and on timeout the original
    // simply takes its normal path, so the worst case is the old behaviour.
    constexpr uint32_t kPreseedFetchTimeoutMs = 5000;

    // When a pre-seed sweep finds manifests that are not archived yet (404), the
    // depots are collected here and, after a short quiet period, surfaced in one
    // MessageBox — so a whole multi-depot / multi-DLC install produces a single
    // "these aren't ready, try again later" prompt instead of one box per depot.
    constexpr int    kMissNotifyDebounceMs = 3000;   // quiet window before showing
    constexpr size_t kMissNotifyMaxLines   = 20;     // cap the listed depots
    std::mutex                                 g_missingMutex;
    std::set<std::pair<uint32_t, uint64_t>>    g_missing;      // pending batch, depot->gid
    std::set<std::pair<uint32_t, uint64_t>>    g_notified;     // already boxed this session
    std::atomic<uint64_t>                      g_missGen{0};   // debounce generation

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    // depotId -> (appId, Steam's own manifest GID). Recorded before the
    // override pass below, so this is what Steam believes rather than what we
    // told it.
    struct DepotSeen { AppId_t appId; uint64 gid; };
    std::unordered_map<uint32, DepotSeen> g_depotsSeen;
    std::mutex g_depotsSeenMutex;

    void RecordDepots(const CUtlVector<DepotEntry>* vec) {
        if (!vec) return;
        std::lock_guard<std::mutex> lock(g_depotsSeenMutex);
        for (uint32 i = 0; i < vec->m_Size; ++i) {
            const DepotEntry& e = vec->m_Memory.m_pMemory[i];
            if (!e.DepotId || !e.ManifestGid) continue;
            g_depotsSeen[e.DepotId] = {e.AppId, e.ManifestGid};
        }
    }

    // Record a depot whose manifest is not archived yet (404) and, after a short
    // quiet window, show a single MessageBox listing everything collected. The
    // debounce (a generation counter) coalesces the burst of a whole install —
    // including the separate BuildDepotDependency calls for a game's DLC apps —
    // into one prompt. The box is shown on this detached waiter, never on Steam's
    // BuildDepotDependency thread, so it cannot freeze Steam.
    void ReportMissing(uint32_t depot, uint64_t gid) {
        {
            std::lock_guard<std::mutex> lock(g_missingMutex);
            // Once per depot per session: an active-but-stuck download re-requests
            // the code every ~30 s, but the user only needs to be told once.
            if (!g_notified.emplace(depot, gid).second) return;
            g_missing.emplace(depot, gid);
        }
        const uint64_t gen = ++g_missGen;

        OSTPlatform::Thread::StartDetached([gen]() -> uint32_t {
            std::this_thread::sleep_for(std::chrono::milliseconds(kMissNotifyDebounceMs));
            // A newer miss arrived during the wait — let its waiter show the box.
            if (g_missGen.load() != gen) return 0;

            std::set<std::pair<uint32_t, uint64_t>> snapshot;
            {
                std::lock_guard<std::mutex> lock(g_missingMutex);
                snapshot.swap(g_missing);
            }
            if (snapshot.empty()) return 0;

            std::string body = std::format(
                "Missing {} manifest(s) from the cache - not archived yet.\n"
                "They've been queued; try the download again in a little while.\n\n"
                "depot;manifestid\n", snapshot.size());

            size_t shown = 0;
            for (const auto& [depot, gid] : snapshot) {
                if (shown >= kMissNotifyMaxLines) {
                    body += std::format("...and {} more", snapshot.size() - shown);
                    break;
                }
                body += std::format("{};{}\n", depot, gid);
                ++shown;
            }

            OSTPlatform::Dialog::ShowWarning("OpenSteamTool - manifests not ready", body);
            return 0;
        });
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFEST_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  [{}] {}", i, DepotEntryDebug(pDepotInfo->m_Memory.m_pMemory[i]));
            }
        }
        if (pSharedDepotInfo) {
            LOG_MANIFEST_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  shared[{}] {}", i, DepotEntryDebug(pSharedDepotInfo->m_Memory.m_pMemory[i]));
            }
        }

        // pbBetaFallback is how steamclient reports "the branch this app asked
        // for could not be resolved, using public instead". The app config
        // writer then persists the manifest WITHOUT its "betakey", which is
        // exactly the "my selected test branch went back to default" symptom —
        // and the reason it hits unlocked games is that their beta-branch
        // manifests are not in the appinfo the client is served, while this
        // tool's fake-license refresh right after logon makes the client do the
        // resolution while appinfo is still in flight.
        //
        // Log what was lost (so manifest.log/beta.log explain it) and let
        // BetaKeeper put the key back once the writer is done with the file.
        if (pbBetaFallback && *pbBetaFallback) {
            const auto branch = BetaKeeper::LastKnownBranch(AppId);
            LOG_MANIFEST_WARN("BuildDepotDependency: app {} reported a beta fallback "
                              "(selected branch {}) — the client would drop the appmanifest "
                              "betakey and move the app off that branch; BetaKeeper answers it",
                              AppId, branch ? *branch : std::string("<none recorded>"));
            BetaKeeper::NotifyBetaFallback(AppId);

            // Answer the fallback with the branch's own build. The authoritative
            // list — which depots and gids that branch is made of — is in the
            // client's appinfo cache; steamclient only refuses to use it for an
            // app it believes it does not own. If appinfo has no row for the
            // branch, the manifest's installed depot set is the next best guess.
            auto applyDepots = [&](const std::vector<BetaKeeper::InstalledDepot>& depots,
                                   const char* source) -> uint32_t {
                uint32_t applied = 0;
                for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                    DepotEntry& entry = pDepotInfo->m_Memory.m_pMemory[i];
                    for (const auto& depot : depots) {
                        if (depot.depotId != entry.DepotId || depot.gid == 0) continue;
                        if (entry.ManifestGid == depot.gid) break;   // already that build
                        LOG_MANIFEST_INFO("BuildDepotDependency: app {} depot {} gid={} -> {} (from {})",
                                          AppId, entry.DepotId, entry.ManifestGid, depot.gid, source);
                        entry.ManifestGid = depot.gid;
                        if (depot.size) entry.ManifestSize = depot.size;
                        ++applied;
                        break;
                    }
                }
                return applied;
            };

            if (*pbBetaFallback && branch && pDepotInfo && pDepotInfo->m_Size) {
                uint32_t applied = applyDepots(BetaKeeper::BranchDepots(AppId, *branch), "appinfo");
                if (!applied) applied = applyDepots(BetaKeeper::InstalledDepots(AppId), "installed build");
                if (applied) {
                    // The depot config now describes the branch's build, so the
                    // build id reported for it has to be that build as well:
                    // leaving the public build id here is what leaves Steam stuck
                    // on "update required" with nothing to download.
                    const uint64_t branchBuildId = BetaKeeper::BranchBuildId(AppId, *branch);
                    if (branchBuildId && pBuildId) {
                        if (*pBuildId != static_cast<uint32>(branchBuildId)) {
                            LOG_MANIFEST_INFO("BuildDepotDependency: app {} build {} -> {} "
                                              "(branch \"{}\")", AppId, *pBuildId,
                                              static_cast<uint32>(branchBuildId), *branch);
                        }
                        *pBuildId = static_cast<uint32>(branchBuildId);
                    }
                    *pbBetaFallback = false;   // the branch is satisfiable; do not waive it
                    LOG_MANIFEST_WARN("BuildDepotDependency: app {} kept branch \"{}\" — {} depot(s) "
                                      "pointed at that branch's build (build {})",
                                      AppId, *branch, applied,
                                      pBuildId ? *pBuildId : 0);
                } else {
                    LOG_MANIFEST_DEBUG("BuildDepotDependency: app {} branch \"{}\" has no depot list "
                                       "in appinfo or in the manifest", AppId, *branch);
                }
            }
        }

        if (!result) return result;

        // Steam reports zero depots for an app during the window where its
        // appinfo is being swapped in by a PICS refresh — the old depot set is
        // dropped before the new one lands. Observed directly for app 3293260:
        // nCount=1 (old gid) -> nCount=0 -> nCount=1 (new gid), ~9s apart. BST
        // widens that window because the fake license adds thousands of apps at
        // once, forcing a large PICS refresh right after login.
        //
        // The caller (steamclient sub_138517140, the depot-config writer) takes
        // whatever comes back and stores it, so acting on the transient writes
        // an EMPTY depot config. The update job then reads that, logs
        // "0 active: 0 target", commits nothing, and still stamps the app Fully
        // Installed at the new BuildID — an install that silently "completes"
        // having downloaded nothing, which is the instant-complete bug.
        //
        // Returning false makes that caller bail on its clean failure path and
        // keep the app's previous depot config, so the next pass (once appinfo
        // has landed) builds it properly. Scoped to apps a lua declares depots
        // for; an app that genuinely has none is left alone.
        //
        // Only pDepotInfo is tested. pSharedDepotInfo holds depots belonging to
        // OTHER apps (sharedinstall / depotfromapp redirects — redistributables
        // and launchers), so it says nothing about whether this app's own config
        // is sound. Testing it too is what let app 3751260 through on 2026-09-20:
        // its own list was empty but one shared entry (depot 228989 of app
        // 228980) was present, so the guard missed and Steam stored the empty
        // config, finishing with "0 mounted depots".
        if (AppId != 0 &&
            (!pDepotInfo || pDepotInfo->m_Size == 0) &&
            LuaConfig::HasDepot(AppId, false))
        {
            LOG_MANIFEST_WARN("BuildDepotDependency: app {} returned 0 depots "
                "(appinfo mid-refresh) — failing the call so Steam keeps its "
                "existing depot config instead of storing an empty one", AppId);
            return false;
        }

        // Before the override pass, so what is cached is Steam's GID.
        RecordDepots(pDepotInfo);
        RecordDepots(pSharedDepotInfo);

        const auto& overrides = LuaConfig::GetManifestOverrides();

        // A Lua manifest set describes ONE build. The Hubcap-generated files
        // most configs come from even stamp that in their header
        // ("-- Build: <id> (public branch)"), and every depot in them is pinned
        // with setManifestid. Forcing those public-branch gids into the depot
        // config of an app Steam has on a test branch is what breaks the branch:
        // steamclient cannot satisfy it ("has N missing manifests for beta ...
        // falling back to public"), falls back to public and persists the app
        // manifest WITHOUT its betakey — so the selection is gone on the next
        // launch. Branch-aware tools avoid this by installing the gids that
        // belong to the branch instead.
        //
        // So while an app sits on a non-default branch, its pins are left alone.
        // [manifest] pins_on_test_branch = true restores the old behaviour for
        // configs whose pins really are the beta's gids.
        const auto testBranch = BetaKeeper::SelectedBranch(AppId);
        const bool mayPin = Config::manifestPinsOnTestBranch || !testBranch;

        // Apply manifest overrides in place (only depots a lua explicitly pins).
        if (!overrides.empty() && mayPin && pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }
        } else if (!overrides.empty() && !mayPin) {
            LOG_MANIFEST_INFO("BuildDepotDependency: app {} is on test branch \"{}\" — leaving its {} "
                              "Lua manifest pin(s) out so the branch can resolve",
                              AppId, *testBranch, overrides.size());
        }

        // No pre-seeding here. It used to run a synchronous sweep over the depot
        // list on Steam's own thread, fetching manifests for every pinned depot
        // while Steam was still building depot configs at startup — speculative
        // work for games that may never be downloaded, and nothing after this
        // point reads the file anyway.
        //
        // YldLoadDepotManifest now covers it properly: it fires at the per-manifest
        // acquire chokepoint, immediately before the original's own disk check, so
        // the fetch happens only for a manifest Steam is asking for right now, and
        // it reaches pinned depots too (the pinned gid was patched into the depot
        // config just above, so that is the gid Steam goes on to request). It also
        // covers what this sweep never could: unpinned depots and workshop items,
        // which do not pass through BuildDepotDependency at all.
        return result;
    }

    // ── Workshop / on-demand manifest pre-seed ──────────────────────────
    // CDepotDownloadMgr's per-manifest acquire (steamclient: sub_1384B97C0). It
    // builds <steam>\depotcache\<depot>_<gid>.manifest, checks disk, and only
    // falls through to BYldRequestDepotManifest (the request-code path) on a
    // miss. Pre-seeding the manifest here — before the original's own disk check
    // — makes that check succeed, so the download starts on the FIRST attempt
    // with no request code. Unlike BuildDepotDependency this fires for every
    // manifest acquisition, including workshop items (depot == appid), which
    // never pass through BuildDepotDependency.
    //
    //   appId=a3, depotId=a4, manifestGid=a5, branch=a6.
    HOOK_FUNC(YldLoadDepotManifest, __int64,
              void* a1, void* a2, int appId, uint32_t depotId,
              uint64_t manifestGid, const char* branch)
    {
        // The branch argument is ground truth for "which branch is this app
        // running right now". Steam rewrites app manifests from scratch during a
        // session and can leave the saved betakey out while it keeps loading this
        // branch, so record it: BetaKeeper restores the key from this instead of
        // having to guess.
        if (branch && *branch) {
            BetaKeeper::NoteActiveBranch(static_cast<AppId_t>(appId), branch);
        }

        // Only depots OST unlocks and does not own (lua-added, incl. workshop
        // apps). A bounded blocking fetch: the original checks disk on the very
        // next line, so an archive hit converts into a first-attempt success; a
        // miss just returns and the original takes its normal request-code path.
        if ( depotId && manifestGid && LuaConfig::HasDepot(depotId) ) {
            LOG_MANIFEST_DEBUG("YldLoadDepotManifest: pre-seed app={} depot={} gid={} branch={}",
                               appId, depotId, manifestGid, branch ? branch : "");
            ManifestCache::EnsureCached(appId, depotId, manifestGid, kPreseedFetchTimeoutMs);
        }
        return oYldLoadDepotManifest(a1, a2, appId, depotId, manifestGid, branch);
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        INSTALL_HOOK_C(YldLoadDepotManifest);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNINSTALL_HOOK(YldLoadDepotManifest);
        UNHOOK_END();
    }

    bool LookupDepot(uint32_t depotId, AppId_t& outAppId, uint64_t& outGid) {
        std::lock_guard<std::mutex> lock(g_depotsSeenMutex);
        auto it = g_depotsSeen.find(depotId);
        if (it == g_depotsSeen.end()) return false;
        outAppId = it->second.appId;
        outGid   = it->second.gid;
        return true;
    }

    void ReportMissingManifest(uint32_t depotId, uint64_t gid) {
        ReportMissing(depotId, gid);
    }
}
