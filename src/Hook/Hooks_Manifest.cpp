#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/SteamMetadata/ManifestCache.h"
#include "Utils/SteamConfig/AppInfoFile.h"
#include "Utils/SteamConfig/AppManifestBeta.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "OSTPlatform/include/Dialog.h"
#include <atomic>
#include <chrono>
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

    // ── debug: what the client hands to BuildDepotDependency ────────────
    // Enabled per app by listing ids in <Steam>/opensteamtool/trace-beta.txt
    // (one per line). Absent file = no effect at all.
    std::filesystem::path SteamRootDir() {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path();
    }

    std::vector<AppId_t> TraceBetaList() {
        std::vector<AppId_t> out;
        std::ifstream in(SteamRootDir() / "opensteamtool" / "trace-beta.txt");
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            try { out.push_back(static_cast<AppId_t>(std::stoul(line))); } catch (...) {}
        }
        return out;
    }

    bool TraceBeta(AppId_t appId) {
        const std::filesystem::path file = SteamRootDir() / "opensteamtool" / "trace-beta.txt";
        std::ifstream in(file);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            try {
                if (static_cast<AppId_t>(std::stoul(line)) == appId) return true;
            } catch (...) {
            }
        }
        return false;
    }

    IKeyValuesSystem* TraceKvSystem() {
        static IKeyValuesSystem* sys = []() -> IKeyValuesSystem* {
            auto vstdlib = OSTPlatform::DynamicLibrary::GetLoaded("vstdlib_s64.dll");
            if (!vstdlib) return nullptr;
            auto pfn = reinterpret_cast<KeyValuesSystemSteam_t>(
                OSTPlatform::DynamicLibrary::GetSymbol(vstdlib, "KeyValuesSystemSteam"));
            return pfn ? pfn() : nullptr;
        }();
        return sys;
    }

    // KeyValues strings may be interned symbols, so never trust the pointer.
    bool SafeCopy(const char* p, char* dst, size_t cap) {
        __try {
            if (!p) return false;
            size_t i = 0;
            for (; i + 1 < cap && p[i]; ++i) dst[i] = p[i];
            dst[i] = 0;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void DumpKv(KeyValues* kv, int depth) {
        auto* sys = TraceKvSystem();
        if (!sys || !kv) return;
        const std::string pad(static_cast<size_t>(depth) * 2, ' ');
        for (KeyValues* child = kv->m_pSub; child; child = child->m_pPeer) {
            const char* rawName = sys->GetStringForSymbol(child->m_iKeyName);
            char name[128] = {};
            SafeCopy(rawName, name, sizeof(name));
            switch (child->m_iDataType) {
                case 0:
                    LOG_MANIFEST_INFO("BETA-KV {}{} {{", pad, name);
                    if (depth < 3) DumpKv(child, depth + 1);
                    break;
                case 1: {
                    char value[160] = {};
                    if (SafeCopy(child->m_sValue, value, sizeof(value)))
                        LOG_MANIFEST_INFO("BETA-KV {}{} = \"{}\"", pad, name, value);
                    else
                        LOG_MANIFEST_INFO("BETA-KV {}{} = <string ptr 0x{:X}>", pad, name,
                                          reinterpret_cast<uintptr_t>(child->m_sValue));
                    break;
                }
                case 2:
                    LOG_MANIFEST_INFO("BETA-KV {}{} = {}", pad, name, child->m_iValue);
                    break;
                case 7:
                    LOG_MANIFEST_INFO("BETA-KV {}{} = {}", pad, name, child->m_ullValue);
                    break;
                default:
                    LOG_MANIFEST_INFO("BETA-KV {}{} (type {})", pad, name,
                                      static_cast<int>(child->m_iDataType));
                    break;
            }
        }
    }

    // Branch the client believes is selected, straight out of the KV it hands to
    // BuildDepotDependency. Measured: the KV carries BetaKey even while the call
    // reports a fallback, so this is the request to honour.
    std::string BranchFromUserConfig(KeyValues* root) {
        auto* sys = TraceKvSystem();
        if (!sys || !root) return {};
        for (KeyValues* child = root->m_pSub; child; child = child->m_pPeer) {
            char name[64] = {};
            if (!SafeCopy(sys->GetStringForSymbol(child->m_iKeyName), name, sizeof(name))) continue;
            if (_stricmp(name, "betakey") != 0) continue;
            if (child->m_iDataType != 1) continue;
            char value[128] = {};
            if (SafeCopy(child->m_sValue, value, sizeof(value)) && value[0]) return value;
        }
        return {};
    }



    // ── appinfo path reads (debug only) ─────────────────────────────────
    // The depot/manifest tables are read through CAppInfoCache::GetAppDataFromAppInfo
    // with a path string like "3450311/manifests/<branch>/gid". Logging the paths
    // shows which branch manifests the client actually asks for.
    HOOK_FUNC(GetAppDataFromAppInfo, int64, void* pCache, AppId_t appId,
              const char* path, uint8* out, int32 outSize) {
        const int64 result = oGetAppDataFromAppInfo(pCache, appId, path, out, outSize);
        if (path && (std::strstr(path, "manifests") || std::strstr(path, "branches") ||
                     std::strstr(path, "-prev_open_beta"))) {
            LOG_MANIFEST_INFO("APPINFO app={} path=\"{}\" -> {} size={}",
                              appId, path, result,
                              out ? std::string(reinterpret_cast<char*>(out),
                                                static_cast<size_t>(outSize > 0 ? outSize : 0)) : std::string());
        }
        return result;
    }


    using KvFindFmt_t = void* (__fastcall*)(void* parent, const char* fmt, bool create,
                                            void* out, void* a4, void* a5);
    KvFindFmt_t oKvFindFmt = nullptr;


    // ── update-job betakey override (experimental) ───────────────────────
    // clientjobupdatedepots decides the beta fallback from the betakey it reads
    // out of the job's own state KV, and that value is already "public" by then
    // (the client normalises it at load). Record the job <-> appid association on
    // entry, then force the appmanifest selection into that KV the moment the job
    // reads it. This is the Branches::Builder-side injection the other tool does.
    using JobUpdate_t = __int64(__fastcall*)(void* job);
    JobUpdate_t oJobUpdate = nullptr;
    std::mutex g_jobMutex;
    std::unordered_map<void*, AppId_t> g_jobApp;

    __int64 __fastcall hkJobUpdate(void* job) {
        AppId_t appId = 0;
        if (job) {
            appId = *reinterpret_cast<AppId_t*>(reinterpret_cast<uint8_t*>(job) + 0x18);
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_jobApp[job] = appId;
        }
        const __int64 result = oJobUpdate(job);
        if (job) {
            std::lock_guard<std::mutex> lock(g_jobMutex);
            g_jobApp.erase(job);
        }
        return result;
    }

    // App owning the state KV the job is reading (0 when it is not a job KV).
    AppId_t AppIdForJobKv(void* kv) {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        for (const auto& [job, appId] : g_jobApp) {
            auto* bytes = static_cast<uint8_t*>(job);
            if (*reinterpret_cast<void**>(bytes + 0xcb8) == kv) return appId;
        }
        return 0;
    }

    void InstallJobHook() {
        {
            const std::filesystem::path flag = SteamRootDir() / "opensteamtool" / "trace-kv.txt";
            std::error_code ec;
            if (!std::filesystem::exists(flag, ec)) return;
        }
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) return;
        static const uint8_t kPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                            0x48, 0x83, 0xEC, 0x50, 0xF6, 0x81, 0xA0, 0x0C, 0x00, 0x00, 0x04};
        uint8_t* target = module + 0x580400;
        if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
            LOG_MANIFEST_WARN("BETA-JOB: prologue mismatch, hook skipped");
            return;
        }
        oJobUpdate = reinterpret_cast<JobUpdate_t>(target);
        if (OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oJobUpdate),
                                        reinterpret_cast<void*>(hkJobUpdate))) {
            LOG_MANIFEST_INFO("BETA-JOB: hooked clientjobupdatedepots (steamclient+0x580400)");
        }
    }

    // ── branch manifest injection (experimental) ─────────────────────────
    // The update job counts "missing manifests for beta" before it builds a
    // depot config, so a branch whose per-depot manifest entries are absent from
    // the app's in-memory appinfo KV is rejected wholesale. Capture the KV node
    // the client creates per app id, then materialise the branch's
    // depots/<depot>/manifests/<branch>/{gid,size} entries with the client's own
    // KeyValues helpers so its own resolution succeeds.
    using KvSetInt_t = void(__fastcall*)(void* kv, int value);
    using KvSetString_t = void(__fastcall*)(void* kv, const char* value);
    KvSetInt_t fnKvSetInt = nullptr;
    KvSetString_t fnKvSetString = nullptr;
    std::mutex g_appKvMutex;
    std::unordered_map<AppId_t, void*> g_appKv;
    std::set<std::string> g_injected;



    std::string SelectedBranchFromManifest(AppId_t appId);

    // ── call the client's own "set beta branch" (experimental) ───────────
    // sub_1384FB840 is what the Betas dialog ends up in: it writes BetaKey into
    // the app's user config KV, logs "AppID %u beta changed: %s" and lets the
    // normal scheduler/update path run. Editing the appmanifest only gets part
    // way there (no user-config-changed event, so nothing is ever queued);
    // calling this gives exactly the behaviour of a UI switch.
    using SetBetaBranch_t = bool(__fastcall*)(void* self, AppId_t appId, const char* branch);
    SetBetaBranch_t fnSetBetaBranch = nullptr;
    std::atomic<void*> g_userAppMgr{nullptr};
    std::atomic_bool g_branchCallStarted{false};

    void TryCallSetBetaBranch(AppId_t appId, const std::string& branch) {
        void* self = g_userAppMgr.load();
        if (!self || branch.empty() || _stricmp(branch.c_str(), "public") == 0) return;
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) return;
        static const uint8_t kPrologue[] = {0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C,
                                            0x24, 0x18, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xA0,
                                            0x00, 0x00, 0x00};
        uint8_t* target = module + 0x4FB840;
        if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
            LOG_MANIFEST_WARN("BETA-SET: prologue mismatch, not calling the branch setter");
            return;
        }
        fnSetBetaBranch = reinterpret_cast<SetBetaBranch_t>(target);
        const bool ok = fnSetBetaBranch(self, appId, branch.c_str());
        LOG_MANIFEST_INFO("BETA-SET: called the client branch setter for app {} -> \"{}\" (ok={})",
                          appId, branch, ok);
    }

    void StartBranchSetterThread() {
        if (!TraceBetaList().empty() == false) return;
        std::thread([]() {
            for (int i = 0; i < 120 && !g_userAppMgr.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!g_userAppMgr.load()) {
                LOG_MANIFEST_WARN("BETA-SET: never captured the app manager, skipping");
                return;
            }
            for (AppId_t appId : TraceBetaList()) {
                const std::string branch = SelectedBranchFromManifest(appId);
                if (!branch.empty()) TryCallSetBetaBranch(appId, branch);
            }
        }).detach();
    }

    bool InjectBranchManifests(AppId_t appId, const std::string& branch,
                               const std::vector<AppInfoFile::BranchDepot>& depots);

    // The branch the user selected, read from the appmanifest. The update job
    // reads the same key out of its own state KV, and it decides the beta
    // fallback before BuildDepotDependency runs, so the manifests have to be in
    // the appinfo KV as soon as that KV exists.
    std::string SelectedBranchFromManifest(AppId_t appId) {
        const std::filesystem::path root = SteamRootDir();
        std::vector<std::filesystem::path> dirs{root / "steamapps"};
        std::ifstream lv(root / "steamapps" / "libraryfolders.vdf");
        if (lv) {
            const std::string text((std::istreambuf_iterator<char>(lv)), std::istreambuf_iterator<char>());
            for (const auto& p : SteamConfig::AppManifestBeta::LibraryFolders(text)) {
                dirs.emplace_back(std::filesystem::path(p) / "steamapps");
            }
        }
        for (const auto& dir : dirs) {
            std::ifstream in(dir / std::format("appmanifest_{}.acf", appId));
            if (!in) continue;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (auto key = SteamConfig::AppManifestBeta::FindBetaKey(text, "UserConfig"); key && !key->empty())
                return *key;
            if (auto key = SteamConfig::AppManifestBeta::FindBetaKey(text, "MountedConfig"); key && !key->empty())
                return *key;
        }
        return {};
    }

    void InjectForKnownApp(AppId_t appId) {
        if (!TraceBeta(appId)) return;
        const std::string branch = SelectedBranchFromManifest(appId);
        if (branch.empty() || _stricmp(branch.c_str(), "public") == 0) return;
        AppInfoFile::Refresh(SteamRootDir().string());
        const auto depots = AppInfoFile::Lookup(appId, branch);
        if (depots.empty()) {
            LOG_MANIFEST_WARN("BETA-INJECT app {} branch \"{}\" has no depot table in appinfo", appId, branch);
            return;
        }
        LOG_MANIFEST_INFO("BETA-INJECT app {} branch \"{}\" depots={} (early, after appinfo KV creation)",
                          appId, branch, depots.size());
        InjectBranchManifests(appId, branch, depots);
    }


    // Find which app a captured appinfo KV node belongs to.
    AppId_t AppIdForKv(void* kv) {
        std::lock_guard<std::mutex> lock(g_appKvMutex);
        for (const auto& [id, node] : g_appKv) {
            if (node == kv) return id;
        }
        return 0;
    }


    bool startsWithTrackedBranch(const char* name, size_t len) {
        if (len == 6 && std::memcmp(name, "public", 6) == 0) return false;
        for (size_t i = 0; i < len; ++i) {
            const char c = name[i];
            if (c == '%' || c == ' ') return false;      // still a format string
        }
        return len > 0;
    }

    void RememberAppKv(const char* key, void* kv) {
        if (!key || !kv) return;
        AppId_t appId = 0;
        for (const char* c = key; *c; ++c) {
            if (*c < '0' || *c > '9') return;      // only plain app-id keys
            appId = appId * 10 + static_cast<AppId_t>(*c - '0');
        }
        if (!appId) return;
        {
            std::lock_guard<std::mutex> lock(g_appKvMutex);
            g_appKv[appId] = kv;
        }
        // Let the cache finish populating this node before writing into it.
        std::thread([appId]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            InjectForKnownApp(appId);
        }).detach();
    }

    void* FindAppKv(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_appKvMutex);
        const auto it = g_appKv.find(appId);
        return it == g_appKv.end() ? nullptr : it->second;
    }

    bool InjectBranchManifests(AppId_t appId, const std::string& branch,
                               const std::vector<AppInfoFile::BranchDepot>& depots) {
        void* appKv = FindAppKv(appId);
        fnKvSetString = reinterpret_cast<KvSetString_t>(
            reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll")) + 0xDA0290);
        fnKvSetInt = reinterpret_cast<KvSetInt_t>(
            reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll")) + 0xDA0250);
        if (!appKv || !oKvFindFmt || !fnKvSetString) return false;

        const std::string key = std::format("{}|{}", appId, branch);
        {
            std::lock_guard<std::mutex> lock(g_appKvMutex);
            if (!g_injected.insert(key).second) return true;   // already done
        }

        bool any = false;
        for (const auto& depot : depots) {
            if (!depot.depotId) continue;
            const std::string gidPath = std::format("depots/{}/manifests/{}/gid", depot.depotId, branch);
            const std::string sizePath = std::format("depots/{}/manifests/{}/size", depot.depotId, branch);
            void* gidNode = oKvFindFmt(appKv, gidPath.c_str(), true, nullptr, nullptr, nullptr);
            void* sizeNode = oKvFindFmt(appKv, sizePath.c_str(), true, nullptr, nullptr, nullptr);
            char buf[32] = {};
            if (gidNode) {
                std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(depot.gid));
                fnKvSetString(gidNode, buf);
                any = true;
            }
            if (sizeNode) {
                std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(depot.size));
                fnKvSetString(sizeNode, buf);
            }
            LOG_MANIFEST_INFO("BETA-INJECT app {} depot {} branch \"{}\" gidNode={} sizeNode={}",
                              appId, depot.depotId, branch,
                              gidNode ? "ok" : "null", sizeNode ? "ok" : "null");
        }
        return any;
    }

    // ── KV lookup trace (debug only) ────────────────────────────────────
    // steamclient reads every branch field through one formatted KeyValues
    // finder: "branches/%s/buildid", "branches/%s/pwdrequired" and the
    // "manifests/<branch>/..." paths the update job needs. Logging what it asks
    // for (and whether the lookup missed) is the only way to tell how a branch
    // resolution fails without a symbol server.
    std::atomic_bool g_kvTraceInstalled{false};

    bool KvTraceWanted() {
        static const bool wanted = []() {
            const std::filesystem::path flag = SteamRootDir() / "opensteamtool" / "trace-kv.txt";
            std::error_code ec;
            return std::filesystem::exists(flag, ec);
        }();
        return wanted;
    }

    bool PrintableArg(void* p, char* dst, size_t cap) {
        __try {
            const auto* s = static_cast<const char*>(p);
            if (!s || !*s) return false;
            size_t i = 0;
            for (; i + 1 < cap && s[i]; ++i) {
                const char c = s[i];
                if (c < 0x20 || c > 0x7e) return false;
                dst[i] = c;
            }
            dst[i] = 0;
            return i > 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void* __fastcall hkKvFindFmt(void* parent, const char* fmt, bool create,
                                 void* out, void* a4, void* a5) {
        // The client resolves a branch by building "branches/<name>/..." paths on
        // the app's appinfo KV; materialise that branch's depot manifests right
        // before it looks them up, which is the one moment early enough for the
        // update job's missing-manifest count.
        if (fmt && !create && parent && std::strncmp(fmt, "branches/", 9) == 0) {
            const AppId_t appId = AppIdForKv(parent);
            if (appId != 0) {
                const char* start = fmt + 9;
                const char* slash = std::strchr(start, '/');
                if (slash && slash > start && startsWithTrackedBranch(start, static_cast<size_t>(slash - start))) {
                    const std::string branch(start, static_cast<size_t>(slash - start));
                    AppInfoFile::Refresh(SteamRootDir().string());
                    const auto depots = AppInfoFile::Lookup(appId, branch);
                    if (!depots.empty()) {
                        LOG_MANIFEST_INFO("BETA-INJECT app {} branch \"{}\" (at branch resolution)", appId, branch);
                        InjectBranchManifests(appId, branch, depots);
                    }
                }
            }
        }
        void* result = oKvFindFmt(parent, fmt, create, out, a4, a5);
        if (create && result) RememberAppKv(fmt, result);
        if (!create && result && fmt &&
            (std::strcmp(fmt, "betakey") == 0 || std::strcmp(fmt, "BetaKey") == 0)) {
            const AppId_t jobApp = AppIdForJobKv(parent);
            if (jobApp != 0) {
                const std::string branch = SelectedBranchFromManifest(jobApp);
                if (!branch.empty() && _stricmp(branch.c_str(), "public") != 0) {
                    fnKvSetString(result, branch.c_str());
                    LOG_MANIFEST_INFO("BETA-JOB: app {} betakey forced to \"{}\" before the job reads it",
                                      jobApp, branch);
                }
            }
            auto* node = static_cast<KeyValues*>(result);
            char value[96] = {};
            const bool ok = node->m_iDataType == 1 && SafeCopy(node->m_sValue, value, sizeof(value));
            LOG_MANIFEST_INFO("BETA-KEY read parent=0x{:X} \"{}\" type={} value={}",
                              reinterpret_cast<uintptr_t>(parent), fmt,
                              static_cast<int>(node->m_iDataType),
                              ok ? value : std::format("raw=0x{:X}",
                                  reinterpret_cast<uintptr_t>(node->m_sValue)));
        }
        bool numeric = fmt && *fmt;
        if (numeric) {
            for (const char* c = fmt; *c; ++c) {
                if (*c < '0' || *c > '9') { numeric = false; break; }
            }
        }
        if (fmt && (numeric || std::strstr(fmt, "manifests") || std::strstr(fmt, "branches") ||
                    std::strstr(fmt, "betakey") || std::strstr(fmt, "-prev_open_beta") ||
                    std::strstr(fmt, "Pinned") || std::strstr(fmt, "open-beta") ||
                    std::strstr(fmt, "testing-branch"))) {
            char a4s[80] = {};
            char a5s[80] = {};
            const bool a4ok = PrintableArg(a4, a4s, sizeof(a4s));
            const bool a5ok = PrintableArg(a5, a5s, sizeof(a5s));
            LOG_MANIFEST_INFO("KVFIND parent=0x{:X} \"{}\" create={} -> {} a4={} a5={}",
                reinterpret_cast<uintptr_t>(parent), fmt, create ? 1 : 0, result ? "hit" : "MISS",
                a4ok ? a4s : std::format("0x{:X}", reinterpret_cast<uintptr_t>(a4)),
                a5ok ? a5s : std::format("0x{:X}", reinterpret_cast<uintptr_t>(a5)));
        }
        return result;
    }

    void InstallKvTrace() {
        if (!KvTraceWanted()) return;
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) {
            LOG_MANIFEST_WARN("KVFIND: steamclient64.dll not loaded");
            return;
        }
        // build 1794536281: sub_138D9EBF0, prologue verified below
        static const uint8_t kPrologue[] = {0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20,
                                            0x57, 0x48, 0x81, 0xEC};
        uint8_t* target = module + 0xD9EBF0;
        if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
            LOG_MANIFEST_WARN("KVFIND: prologue mismatch at steamclient+0x{:X}, hook skipped", 0xD9EBF0u);
            return;
        }
        oKvFindFmt = reinterpret_cast<KvFindFmt_t>(target);
        if (!OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oKvFindFmt),
                                         reinterpret_cast<void*>(hkKvFindFmt))) {
            LOG_MANIFEST_WARN("KVFIND: detour attach failed");
            return;
        }
        g_kvTraceInstalled.store(true);
        LOG_MANIFEST_INFO("KVFIND: hooked steamclient+0x{:X} (formatted KeyValues lookup)", 0xD9EBF0u);
    }

    // Steam's update job decides what to download from the target depot manifests
    // in <Steam>/depotcache. When a branch is answered here and that branch's
    // manifests were never fetched, the job finds nothing to compare, logs
    // "has no changes" and stamps the new build id without downloading anything
    // (measured: switching to 1.4.3 recorded build 26105915 while the game folder
    // still held 1.4.2's 13,941,903,965 bytes and depotcache had no 1.4.3
    // manifests). Fetch them here, once per app+branch, so the job that follows
    // has real data to diff.
    void PrefetchBranchManifests(AppId_t appId, const std::string& branch,
                                 const std::vector<AppInfoFile::BranchDepot>& depots) {
        static std::mutex mutex;
        static std::set<std::string> done;
        const std::string key = std::format("{}|{}", appId, branch);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!done.insert(key).second) return;
        }
        std::thread([appId, branch, depots]() {
            for (const auto& depot : depots) {
                if (depot.depotId == 0 || depot.gid == 0) continue;
                ManifestCache::EnsureCached(appId, depot.depotId, depot.gid,
                                            5000, nullptr, true);
                LOG_MANIFEST_INFO("BETA: prefetched manifest for app {} depot {} gid {} (branch \"{}\")",
                    appId, depot.depotId, depot.gid, branch);
            }
        }).detach();
    }

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
        if (AppId != 0 && !g_userAppMgr.load()) {
            g_userAppMgr.store(pUserAppMgr);
            std::thread([]() {
                for (int i = 0; i < 40 && !g_branchCallStarted.exchange(true); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                StartBranchSetterThread();
            }).detach();
        }
        if (TraceBeta(AppId) && AppId != 0) {
            const std::string branch = BranchFromUserConfig(reinterpret_cast<KeyValues*>(pUserConfig));
            if (!branch.empty() && _stricmp(branch.c_str(), "public") != 0) {
                AppInfoFile::Refresh(SteamRootDir().string());
                const auto depots = AppInfoFile::Lookup(AppId, branch);
                if (!depots.empty()) InjectBranchManifests(AppId, branch, depots);
            }
        }
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);

        if (TraceBeta(AppId)) {
            LOG_MANIFEST_INFO("BETA-TRACE app {} result={} build={} fallback={} depots={} shared={}",
                AppId, result, pBuildId ? *pBuildId : 0,
                pbBetaFallback ? *pbBetaFallback : false,
                pDepotInfo ? pDepotInfo->m_Size : 0,
                pSharedDepotInfo ? pSharedDepotInfo->m_Size : 0);
            DumpKv(reinterpret_cast<KeyValues*>(pUserConfig), 0);
        }
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

        if (!result) return result;

        // Steam has the selected branch in memory (the KV below proves it) yet
        // still reports a beta fallback and hands back the public build: for an
        // app it does not own it will not resolve branch depots by itself. Answer
        // that fallback with the branch's own depot table from the appinfo cache
        // and clear the flag, so the client keeps the selection instead of
        // rewriting the manifest without it.
        if (pbBetaFallback && *pbBetaFallback && AppId != 0 &&
            Config::GetManifestAnswerBetaFallback()) {
            const std::string branch = BranchFromUserConfig(reinterpret_cast<KeyValues*>(pUserConfig));
            if (!branch.empty() && _stricmp(branch.c_str(), "public") != 0) {
                AppInfoFile::Refresh(SteamRootDir().string());
                const auto branchDepots = AppInfoFile::Lookup(AppId, branch);
                const auto branchBuild = AppInfoFile::LookupBuildId(AppId, branch);
                if (!branchDepots.empty()) {
                    uint32_t patched = 0;
                    if (pDepotInfo) {
                        for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                            DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                            for (const auto& d : branchDepots) {
                                if (d.depotId != e.DepotId) continue;
                                if (e.ManifestGid != d.gid) {
                                    LOG_MANIFEST_INFO("BETA: app {} depot {} gid {} -> {} (branch \"{}\")",
                                        AppId, e.DepotId, e.ManifestGid, d.gid, branch);
                                    e.ManifestGid = d.gid;
                                }
                                if (d.size != 0) e.ManifestSize = d.size;
                                ++patched;
                                break;
                            }
                        }
                    }
                    if (branchBuild != 0 && pBuildId && *pBuildId != static_cast<uint32>(branchBuild)) {
                        LOG_MANIFEST_INFO("BETA: app {} build {} -> {} (branch \"{}\")",
                            AppId, *pBuildId, branchBuild, branch);
                        *pBuildId = static_cast<uint32>(branchBuild);
                    }
                    *pbBetaFallback = false;
                    PrefetchBranchManifests(AppId, branch, branchDepots);
                    LOG_MANIFEST_INFO("BETA: app {} answered the beta fallback for branch \"{}\" "
                                      "({} depot(s) matched, {} from appinfo)",
                        AppId, branch, patched, branchDepots.size());
                } else {
                    LOG_MANIFEST_WARN("BETA: app {} requested branch \"{}\" but appinfo has no "
                                      "depot table for it — leaving the fallback to Steam",
                        AppId, branch);
                }
            }
        }

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

        // Apply manifest overrides in place (only depots a lua explicitly pins).
        if (!overrides.empty() && pDepotInfo && pDepotInfo->m_Size) {
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
        INSTALL_HOOK_C(GetAppDataFromAppInfo);
        InstallKvTrace();
        InstallJobHook();          // attaches inside the same transaction
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
