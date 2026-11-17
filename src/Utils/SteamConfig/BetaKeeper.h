#pragma once

// Keeps a game's selected test branch (beta) alive across Steam launches.
//
// Why this exists: steamclient persists the choice as "betakey" in
// steamapps/appmanifest_<appid>.acf ("UserConfig"). When it cannot resolve that
// branch for the app it rewrites the manifest WITHOUT the key, so the branch
// silently reverts to public on the next start — the same silent removal
// reported for steamcmd in ValveSoftware/steam-for-linux#9076.
//
// Unlocked (not really owned) games hit that path routinely: their beta-branch
// manifests are not in the appinfo the client is served, and the fake-license
// refresh this tool triggers right after logon makes the client resolve the
// branch while appinfo is still in flight (see the "0 depots / appinfo
// mid-refresh" note in Hooks_Manifest.cpp). The client then logs
// "has N missing manifests for beta ... falling back to public".
//
// The keeper scans the installed app manifests, remembers each app's last
// non-empty betakey, and writes it back when Steam drops it: either on request
// (the depot-config hook saw a beta fallback for that app) or because a Lua
// config pins the branch with keepBeta(appid, "openbeta").
//
// A repair touches only "betakey" inside "UserConfig" — no other key in the
// manifest is rewritten (see Utils/SteamConfig/AppManifestBeta.h).

#include "Steam/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace BetaKeeper {

    // One depot of the build installed for an app (its manifest's InstalledDepots).
    struct InstalledDepot {
        AppId_t  depotId = 0;
        uint64_t gid     = 0;
        uint64_t size    = 0;
    };

    // keepBeta(appid, branch): re-assert `branch` for this app whenever the
    // manifest loses it. An empty `branch` clears the pin instead.
    void Pin(AppId_t appId, std::string branch);
    void Unpin(AppId_t appId);
    std::optional<std::string> Pinned(AppId_t appId);

    // Last non-empty betakey seen on disk for the app; nullopt when the app was
    // never seen installed. Used for log messages and as the restore value.
    std::optional<std::string> LastKnownBranch(AppId_t appId);

    // Branch Steam currently has the app on, following the manifest: nullopt as
    // soon as the file no longer carries a betakey. This is the "is this app on
    // a test branch?" signal used to stop re-applying public-branch manifest
    // pins over a beta branch's depot config.
    std::optional<std::string> SelectedBranch(AppId_t appId);

    // Depot set of the build currently installed for the app, read from its
    // manifest. Used when steamclient cannot resolve the selected branch: the
    // machine is still on that branch's build, so its gids are what the depot
    // config should keep instead of rolling back to the public build.
    std::vector<InstalledDepot> InstalledDepots(AppId_t appId);

    // Depot set last seen for (app, branch), remembered across launches in
    // <Steam>/opensteamtool/beta-depots.txt. steamclient drops a branch's depot
    // manifests for apps it believes it does not own, so the build has to be
    // recalled from when the branch really was installed.
    std::vector<InstalledDepot> BranchDepots(AppId_t appId, std::string_view branch);

    // Build id that branch points at, from the client's appinfo cache (0 when
    // unknown). BuildDepotDependency reports this through pBuildId, so a depot
    // config that was moved onto the branch has to move this value with it.
    uint64_t BranchBuildId(AppId_t appId, std::string_view branch);

    // Record one branch manifest entry taken from the client's own appinfo
    // ("depots/<depot>/manifests/<branch>/{gid,size}"). This is the authoritative
    // branch -> build mapping: it is what Steam itself was served, so it cannot
    // be confused with the public build the way an app manifest can.
    void NoteBranchManifest(AppId_t appId, std::string_view branch, AppId_t depotId,
                            uint64_t gid, uint64_t size);

    // True when the app is one BetaKeeper tracks (its manifest has been seen, or
    // it is pinned): cheap filter so the appinfo walk touches only wanted apps.
    bool IsTracked(AppId_t appId);

    // steamclient said it fell back to the public branch for this app
    // (BuildDepotDependency's pbBetaFallback). Marks the app so the next scan
    // restores its betakey if Steam waived it. No file I/O on the caller thread.
    void NotifyBetaFallback(AppId_t appId);

    // steamclient is loading depot manifests for `branch` (the branch argument of
    // YldLoadDepotManifest): the app really is running that test branch right
    // now. If the manifest later loses its betakey while this is still true,
    // the drop was Steam rewriting its config, not the user choosing public — so
    // the key is restored. No file I/O on the caller thread.
    void NoteActiveBranch(AppId_t appId, std::string_view branch);

    // Ask for a few quick scan passes. Called right after the injected license is
    // re-asserted, because that is when steamclient rewrites the affected apps'
    // manifests — measured: the betakey is gone within the same second as the
    // startup ownership pass (22:32:20 wipe vs 22:32:20 ownership query), and not
    // at all when this tool's hooks are disabled. Cheap and thread-safe.
    void RequestScan();

    // Start the background scanner once. Call after the Lua config has been
    // parsed (pins come from there). No-op when already running. Repairs are
    // still performed with no pins configured: the fallback notification alone
    // is enough to restore a branch Steam dropped.
    void Start();
}
