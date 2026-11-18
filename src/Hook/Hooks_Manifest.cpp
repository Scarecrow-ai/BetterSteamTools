#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/SteamMetadata/ManifestCache.h"
#include "Utils/SteamConfig/AppInfoFile.h"
#include "Utils/SteamConfig/AppManifestBeta.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include <windows.h>
#include <winreg.h>
#include <atomic>
#include <cstring>
#include <functional>
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
    // The appmanifest keeps the selection nested (UserConfig/BetaKey,
    // MountedConfig/BetaKey), so a shallow scan always came back empty and the
    // depot answer never fired.
    std::string BranchFromUserConfig(KeyValues* root) {
        auto* sys = TraceKvSystem();
        if (!sys || !root) return {};
        std::string found;
        std::function<void(KeyValues*, int)> scan = [&](KeyValues* node, int depth) {
            if (!found.empty()) return;
            for (KeyValues* child = node->m_pSub; child; child = child->m_pPeer) {
                char name[64] = {};
                if (!SafeCopy(sys->GetStringForSymbol(child->m_iKeyName), name, sizeof(name))) continue;
                if (_stricmp(name, "betakey") == 0 && child->m_iDataType == 1) {
                    char value[128] = {};
                    if (SafeCopy(child->m_sValue, value, sizeof(value)) && value[0]) {
                        found = value;
                        return;
                    }
                }
                // m_pSub is only a child pointer for section nodes; on value
                // nodes it aliases the value itself, and following it crashes.
                if (depth > 0 && child->m_iDataType == 0 && child->m_pSub) scan(child, depth - 1);
            }
        };
        scan(root, 2);
        return found;
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

    // The client builds the pinned branch's depot manifests itself and writes the
    // public gid/size into them, overwriting ours. Remember those nodes and answer
    // the write with the beta's values instead.
    struct PinnedNodeInfo {
        AppId_t appId = 0;
        uint64_t depotId = 0;
        bool isSize = false;
    };
    std::mutex g_pinnedNodesMutex;
    std::unordered_map<void*, PinnedNodeInfo> g_pinnedNodes;

    bool ParsePinnedManifestPath(const char* fmt, uint64_t& depotId, bool& isSize) {
        if (!fmt) return false;
        const std::string path(fmt);
        const size_t manifestPos = path.find("manifests/Pinned/");
        if (manifestPos == std::string::npos) return false;
        if (path.compare(path.size() >= 4 ? path.size() - 4 : 0, 4, "/gid") == 0) {
            isSize = false;
        } else if (path.size() >= 5 && path.compare(path.size() - 5, 5, "/size") == 0) {
            isSize = true;
        } else {
            return false;
        }
        std::string head = path.substr(0, manifestPos);
        if (head.rfind("depots/", 0) == 0) head = head.substr(7);
        while (!head.empty() && head.back() == '/') head.pop_back();
        if (head.empty()) return false;
        for (char c : head) {
            if (c < '0' || c > '9') return false;
        }
        depotId = std::strtoull(head.c_str(), nullptr, 10);
        return depotId != 0;
    }

    std::vector<std::pair<AppId_t, std::string>> PinnedSources();
    std::string PinnedSourceFor(AppId_t appId);
    bool SwapPublicManifestTable(AppId_t appId, const std::string& sourceBranch, bool install);
    bool InjectPinnedBranch(AppId_t appId, const std::string& sourceBranch);



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
            if (TraceBeta(appId)) {
                LOG_MANIFEST_INFO("BETA-JOB: update job entered for app {} (job=0x{:X} kv=0x{:X})",
                                  appId, reinterpret_cast<uintptr_t>(job),
                                  *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(job) + 0xcb8));
            }
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
        // The job may read betakey from a KV it holds elsewhere; with a single job
        // in flight its app is still unambiguous.
        if (g_jobApp.size() == 1) {
            const AppId_t only = g_jobApp.begin()->second;
            if (TraceBeta(only)) {
                LOG_MANIFEST_INFO("BETA-JOB: betakey read by the active job (app {}), kv=0x{:X}",
                                  only, reinterpret_cast<uintptr_t>(kv));
            }
            return only;
        }
        return 0;
    }

    void InstallJobHook() {
        {
            const std::filesystem::path flag = SteamRootDir() / "opensteamtool" / "trace-kv.txt";
            std::error_code ec;
            if (!std::filesystem::exists(flag, ec) && !Config::GetManifestInjectBranchManifests()) return;
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


    // ── who writes "public" into the app state (debug only) ──────────────
    // The branch selection is normalised to public before the update job runs and
    // before the appinfo KeyValues exists. Log the stack of every KeyValues string
    // write of interest during startup so the deciding function can be identified.
    using KvSetString_t = void(__fastcall*)(void* kv, const char* value);
    KvSetString_t oKvSetString = nullptr;
    std::atomic_int g_kvSetStringCalls{0};

    void __fastcall hkKvSetString(void* kv, const char* value) {
        if (kv) {
            PinnedNodeInfo info{};
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(g_pinnedNodesMutex);
                const auto it = g_pinnedNodes.find(kv);
                if (it != g_pinnedNodes.end()) {
                    info = it->second;
                    have = true;
                }
            }
            if (have) {
                const std::string source = PinnedSourceFor(info.appId);
                if (!source.empty()) {
                    AppInfoFile::Refresh(SteamRootDir().string());
                    for (const auto& depot : AppInfoFile::Lookup(info.appId, source)) {
                        if (depot.depotId != info.depotId) continue;
                        char buf[32] = {};
                        std::snprintf(buf, sizeof(buf), "%llu",
                                      static_cast<unsigned long long>(info.isSize ? depot.size : depot.gid));
                        LOG_MANIFEST_INFO("BETA-PIN: app {} depot {} pinned {} answered with {}",
                                          info.appId, info.depotId, info.isSize ? "size" : "gid", buf);
                        if (oKvSetString) oKvSetString(kv, buf);
                        return;
                    }
                }
            }
        }
        if (value && oKvSetString && g_kvSetStringCalls.load() < 20000) {
            ++g_kvSetStringCalls;
            const bool interesting = std::strcmp(value, "public") == 0 ||
                std::strcmp(value, "26655995") == 0 || std::strcmp(value, "26105915") == 0 ||
                std::strstr(value, "prev_open_beta") != nullptr;
            if (interesting) {
                void* frames[14] = {};
                const USHORT n = RtlCaptureStackBackTrace(2, 14, frames, nullptr);
                const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"steamclient64.dll"));
                std::string stack;
                for (USHORT i = 0; i < n; ++i) {
                    const auto addr = reinterpret_cast<uintptr_t>(frames[i]);
                    const bool inside = addr > module && addr < module + 0x2000000;
                    stack += inside ? std::format(" +0x{:X}", addr - module)
                                    : std::format(" 0x{:X}", addr);
                }
                LOG_MANIFEST_INFO("KV-SETSTR value=\"{}\" stack:{}", value, stack);
            }
        }
        oKvSetString(kv, value);
    }

    void InstallKvSetTrace() {
        {
            const std::filesystem::path flag = SteamRootDir() / "opensteamtool" / "trace-kv.txt";
            std::error_code ec;
            if (!std::filesystem::exists(flag, ec)) return;
        }
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) return;
        uint8_t* target = module + 0xDA0290;
        if (target[0] != 0x48 || target[1] != 0x85 || target[2] != 0xC9) {
            LOG_MANIFEST_WARN("KV-SETSTR: prologue mismatch at +0xDA0290");
            return;
        }
        oKvSetString = reinterpret_cast<KvSetString_t>(target);
        if (OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oKvSetString),
                                        reinterpret_cast<void*>(hkKvSetString))) {
            LOG_MANIFEST_INFO("KV-SETSTR: hooked steamclient+0xDA0290");
        }
    }


    std::string SelectedBranchFromManifest(AppId_t appId);


    bool EnsureBetahashValue(AppId_t appId, const std::string& branch);


    // ── register the selected branch the way the client itself does ──────
    // sub_1384F5DD0 enumerates HKCU\Software\Valve\Steam\Apps\<appid> and keeps
    // only values named "betahash_<branch>"; the branch name comes from that
    // suffix, and a branch with no such value is never considered. An app the
    // account does not own therefore has no betas at all. Writing the value the
    // client would have written makes its own resolution see the branch.
    bool EnsureBetahashValue(AppId_t appId, const std::string& branch) {
        if (!appId || branch.empty() || _stricmp(branch.c_str(), "public") == 0) return false;
        const std::wstring subkey = std::format(L"Software\\Valve\\Steam\\Apps\\{}", appId);
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0,
                            KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            LOG_MANIFEST_WARN("BETA-REG: cannot open HKCU\\{}", std::string(subkey.begin(), subkey.end()));
            return false;
        }
        const std::wstring valueName = L"betahash_" + std::wstring(branch.begin(), branch.end());
        wchar_t existing[8] = {};
        DWORD size = sizeof(existing);
        DWORD type = 0;
        if (RegQueryValueExW(key, valueName.c_str(), nullptr, &type,
                             reinterpret_cast<LPBYTE>(existing), &size) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
        const char placeholder[] = "0";
        const LONG written = RegSetValueExW(key, valueName.c_str(), 0, REG_SZ,
                                            reinterpret_cast<const BYTE*>(placeholder), sizeof(placeholder));
        RegCloseKey(key);
        LOG_MANIFEST_INFO("BETA-REG: app {} branch \"{}\" registered (result={})", appId, branch, written);
        return written == ERROR_SUCCESS;
    }


    // 0x4EA680 is the same "is this branch cached for the account" reader the
    // update job uses before it decides a private beta is unusable. The reference
    // tool grants its synthetic branch here as well; without it the job rewrites
    // the betakey back to public ("missing cached password for private beta").
    using BetaHashRead_t = bool(__fastcall*)(void* store, AppId_t appId, const char* branch);
    BetaHashRead_t oBetaHashRead = nullptr;

    bool __fastcall hkBetaHashRead(void* store, AppId_t appId, const char* branch) {
        if (branch && *branch &&
            (Config::GetManifestInjectBranchManifests() || TraceBeta(appId)) &&
            _stricmp(branch, "Pinned") == 0 && !PinnedSourceFor(appId).empty()) {
            return true;
        }
        return oBetaHashRead ? oBetaHashRead(store, appId, branch) : false;
    }

    void InstallBetaHashReadGrant() {
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) return;
        uint8_t* target = module + 0x4EA680;
        static const uint8_t kPrologue[] = {0x40, 0x53, 0x48, 0x81, 0xEC, 0xD0, 0x01, 0x00, 0x00};
        if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
            LOG_MANIFEST_WARN("BETA-GRANT: prologue mismatch at +0x4EA680");
            return;
        }
        oBetaHashRead = reinterpret_cast<BetaHashRead_t>(target);
        if (OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oBetaHashRead),
                                        reinterpret_cast<void*>(hkBetaHashRead))) {
            LOG_MANIFEST_INFO("BETA-GRANT: hooked the cached-password reader (steamclient+0x4EA680)");
        }
    }


    // 0x561CC0 answers "what is the state of this app's beta token", and the
    // update job treats 5 (invalid password) as unusable and rewrites the betakey
    // to public. The token lives on Steam's backend, so a synthetic branch can
    // never obtain one; the reference tool answers the server response itself.
    using BetaToken_t = int(__fastcall*)(void* self, AppId_t appId, const char* branch);
    BetaToken_t oBetaToken = nullptr;

    int __fastcall hkBetaToken(void* self, AppId_t appId, const char* branch) {
        // Short circuit before the original runs: it logs the invalid-password
        // warning and rewrites the betakey back to public itself, so patching the
        // return value afterwards is too late.
        if (branch && *branch &&
            (Config::GetManifestInjectBranchManifests() || TraceBeta(appId)) &&
            _stricmp(branch, "Pinned") == 0 && !PinnedSourceFor(appId).empty()) {
            LOG_MANIFEST_INFO("BETA-GRANT: app {} beta token for \"Pinned\" short-circuited to valid", appId);
            return 0;
        }
        return oBetaToken ? oBetaToken(self, appId, branch) : 0;
    }

    void InstallBetaTokenGrant() {
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) return;
        uint8_t* target = module + 0x561CC0;
        static const uint8_t kPrologue[] = {0x40, 0x53, 0x57, 0x41, 0x55, 0x41, 0x56,
                                            0x48, 0x81, 0xEC, 0x58, 0x01};
        if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
            LOG_MANIFEST_WARN("BETA-GRANT: prologue mismatch at +0x561CC0");
            return;
        }
        oBetaToken = reinterpret_cast<BetaToken_t>(target);
        if (OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oBetaToken),
                                        reinterpret_cast<void*>(hkBetaToken))) {
            LOG_MANIFEST_INFO("BETA-GRANT: hooked the beta token check (steamclient+0x561CC0)");
        }
    }

    // ── beta access grant (HubcapTools "Branches::BetaAccessCheck") ──────
    // steamclient+0x4EACE0 decides whether an app may use a beta branch by
    // reading Software\Valve\Steam\<root>\Apps\<appid>\betahash_<branch>.
    // An app the account does not own has no such value, so the check fails and
    // the client falls back to public. The reference tool hooks exactly this
    // check and grants its synthetic branch; do the same for the branch the
    // appmanifest selects.
    using BetaAccessCheck_t = bool(__fastcall*)(void* self, AppId_t appId, const char* branch);
    BetaAccessCheck_t oBetaAccessCheck = nullptr;

    bool __fastcall hkBetaAccessCheck(void* self, AppId_t appId, const char* branch) {
        const bool granted = oBetaAccessCheck ? oBetaAccessCheck(self, appId, branch) : false;
        if (granted) return true;
        if (!branch || !*branch || _stricmp(branch, "public") == 0) return false;
        if (!Config::GetManifestInjectBranchManifests() && TraceBetaList().empty()) return false;
        // our synthetic branch: the client has no betahash for it unless it was
        // written, and the reference tool grants it outright the same way.
        if (_stricmp(branch, "Pinned") == 0 && !PinnedSourceFor(appId).empty()) {
            LOG_MANIFEST_INFO("BETA-GRANT: app {} branch \"Pinned\" granted", appId);
            return true;
        }
        const std::string selected = SelectedBranchFromManifest(appId);
        if (selected.empty() || _stricmp(selected.c_str(), branch) != 0) return false;
        LOG_MANIFEST_INFO("BETA-GRANT app {} branch \"{}\" granted (client denied it)", appId, branch);
        return true;
    }

    void InstallBetaAccessGrant() {
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) return;
        uint8_t* target = module + 0x4EACE0;
        static const uint8_t kPrologue[] = {0x48, 0x81, 0xEC, 0xC8, 0x01, 0x00, 0x00};
        if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
            LOG_MANIFEST_WARN("BETA-GRANT: prologue mismatch at +0x4EACE0");
            return;
        }
        oBetaAccessCheck = reinterpret_cast<BetaAccessCheck_t>(target);
        if (OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oBetaAccessCheck),
                                        reinterpret_cast<void*>(hkBetaAccessCheck))) {
            LOG_MANIFEST_INFO("BETA-GRANT: hooked the beta access check (steamclient+0x4EACE0)");
        }
    }



    // 0xDA0340 is KeyValues::SetUint64; the client writes the pinned branch's
    // depot gid/size through it, so the string hook never saw those values.
    using KvSetUint64_t = void(__fastcall*)(void* kv, uint64_t value);
    KvSetUint64_t oKvSetUint64 = nullptr;

    void __fastcall hkKvSetUint64(void* kv, uint64_t value) {
        if (kv) {
            PinnedNodeInfo info{};
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(g_pinnedNodesMutex);
                const auto it = g_pinnedNodes.find(kv);
                if (it != g_pinnedNodes.end()) {
                    info = it->second;
                    have = true;
                }
            }
            if (have) {
                const std::string source = PinnedSourceFor(info.appId);
                if (!source.empty()) {
                    AppInfoFile::Refresh(SteamRootDir().string());
                    for (const auto& depot : AppInfoFile::Lookup(info.appId, source)) {
                        if (depot.depotId != info.depotId) continue;
                        const uint64_t pinned = info.isSize ? depot.size : depot.gid;
                        LOG_MANIFEST_INFO("BETA-PIN: app {} depot {} pinned {} written as {} (was {})",
                                          info.appId, info.depotId, info.isSize ? "size" : "gid",
                                          pinned, value);
                        if (oKvSetUint64) oKvSetUint64(kv, pinned);
                        return;
                    }
                }
            }
        }
        if (oKvSetUint64) oKvSetUint64(kv, value);
    }

    void InstallKvSetUint64Hook() {
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll"));
        if (!module) return;
        uint8_t* target = module + 0xDA0340;
        static const uint8_t kPrologue[] = {0x48, 0x85, 0xC9, 0x74};
        if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
            LOG_MANIFEST_WARN("BETA-PIN: prologue mismatch at +0xDA0340");
            return;
        }
        oKvSetUint64 = reinterpret_cast<KvSetUint64_t>(target);
        if (OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oKvSetUint64),
                                        reinterpret_cast<void*>(hkKvSetUint64))) {
            LOG_MANIFEST_INFO("BETA-PIN: hooked KeyValues::SetUint64 (steamclient+0xDA0340)");
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

    void* FindAppKv(AppId_t appId);
    bool InjectBranchManifests(AppId_t appId, const std::string& branch,
                               const std::vector<AppInfoFile::BranchDepot>& depots);

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
        // The switch only sticks if the branch is resolvable at that moment: the
        // client validates the new branch right away and otherwise keeps the one
        // it already had.
        // "Pinned" is our own branch: it has no appinfo row, it was built by
        // InjectPinnedBranch from the recorded source branch.
        if (_stricmp(branch.c_str(), "Pinned") == 0) {
            const std::string source = PinnedSourceFor(appId);
            if (source.empty()) {
                LOG_MANIFEST_WARN("BETA-PIN: app {} has no pin.txt source, not switching", appId);
                return;
            }
            for (int i = 0; i < 40 && !FindAppKv(appId); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!InjectPinnedBranch(appId, source)) {
                LOG_MANIFEST_WARN("BETA-PIN: app {} could not build Pinned from \"{}\"", appId, source);
                return;
            }
            EnsureBetahashValue(appId, "Pinned");
            SwapPublicManifestTable(appId, source, true);
            const bool pinnedOk = fnSetBetaBranch(self, appId, "Pinned");
            std::thread([appId, source]() {
                std::this_thread::sleep_for(std::chrono::seconds(8));
                SwapPublicManifestTable(appId, source, false);
            }).detach();
            LOG_MANIFEST_INFO("BETA-PIN: app {} switched to Pinned (source \"{}\") ok={}",
                              appId, source, pinnedOk);
            return;
        }
        bool injected = false;
        for (int i = 0; i < 40 && !injected; ++i) {
            if (!FindAppKv(appId)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            AppInfoFile::Refresh(SteamRootDir().string());
            const auto depots = AppInfoFile::Lookup(appId, branch);
            if (depots.empty()) {
                LOG_MANIFEST_WARN("BETA-SET: app {} branch \"{}\" is not in appinfo, not switching",
                                  appId, branch);
                return;
            }
            LOG_MANIFEST_INFO("BETA-SET: injecting app {} branch \"{}\" ({}) before the switch",
                              appId, branch, depots.size());
            injected = InjectBranchManifests(appId, branch, depots);
            if (!injected) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!injected) {
            LOG_MANIFEST_WARN("BETA-SET: app {} could not inject branch \"{}\" — not switching",
                              appId, branch);
            return;
        }
        // The client only considers branches whose betahash_ value exists, and it
        // writes that value itself when a beta is joined for real.
        EnsureBetahashValue(appId, branch);
        const bool ok = fnSetBetaBranch(self, appId, branch.c_str());
        LOG_MANIFEST_INFO("BETA-SET: called the client branch setter for app {} -> \"{}\" (ok={})",
                          appId, branch, ok);
    }


    // Every installed app whose manifest selects a non-public branch. This is the
    // list the client-side switch drives, so no per-app opt-in file is needed.
    std::vector<std::pair<AppId_t, std::string>> AppsWithTestBranch() {
        std::vector<std::pair<AppId_t, std::string>> out;
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
            std::error_code ec;
            std::filesystem::directory_iterator it(dir, ec);
            for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (name.rfind("appmanifest_", 0) != 0 || it->path().extension() != ".acf") continue;
                std::ifstream in(it->path());
                if (!in) continue;
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto key = SteamConfig::AppManifestBeta::FindBetaKey(text, "UserConfig");
                if (!key || key->empty() || _stricmp(key->c_str(), "public") == 0) continue;
                AppId_t appId = 0;
                const size_t at = text.find("\"appid\"");
                if (at != std::string::npos) {
                    size_t p2 = text.find('"', at + 7);
                    if (p2 != std::string::npos) {
                        ++p2;
                        size_t p3 = text.find('"', p2);
                        if (p3 != std::string::npos) appId = static_cast<AppId_t>(std::strtoul(text.substr(p2, p3 - p2).c_str(), nullptr, 10));
                    }
                }
                if (appId) out.emplace_back(appId, *key);
            }
        }
        return out;
    }


    // "appid=branch" lines: apply a branch without the Betas dialog. This is both
    // a headless way to switch and how the client-side path is exercised in tests.
    std::vector<std::pair<AppId_t, std::string>> DesiredBranches() {
        std::vector<std::pair<AppId_t, std::string>> out;
        std::ifstream in(SteamRootDir() / "opensteamtool" / "desired-branch.txt");
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const AppId_t appId = static_cast<AppId_t>(std::strtoul(line.substr(0, eq).c_str(), nullptr, 10));
            std::string branch = line.substr(eq + 1);
            if (appId && !branch.empty()) out.emplace_back(appId, branch);
        }
        return out;
    }

    void StartBranchSetterThread() {
        if (!Config::GetManifestSwitchBranchViaClient() && TraceBetaList().empty()) return;
        std::thread([]() {
            for (int i = 0; i < 120 && !g_userAppMgr.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (!g_userAppMgr.load()) {
                LOG_MANIFEST_WARN("BETA-SET: never captured the app manager, skipping");
                return;
            }
            const auto pinned = PinnedSources();
            if (!pinned.empty()) {
                for (const auto& [appId, source] : pinned) {
                    if (!FindAppKv(appId)) {
                        for (int i = 0; i < 40 && !FindAppKv(appId); ++i)
                            std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                    if (!InjectPinnedBranch(appId, source)) {
                        LOG_MANIFEST_WARN("BETA-PIN: app {} could not build Pinned from \"{}\"",
                                          appId, source);
                        continue;
                    }
                    EnsureBetahashValue(appId, "Pinned");
                    TryCallSetBetaBranch(appId, "Pinned");
                }
                return;
            }
            const auto desired = DesiredBranches();
            if (!desired.empty()) {
                for (const auto& [appId, branch] : desired) TryCallSetBetaBranch(appId, branch);
                return;
            }
            if (Config::GetManifestSwitchBranchViaClient()) {
                for (const auto& [appId, branch] : AppsWithTestBranch())
                    TryCallSetBetaBranch(appId, branch);
            } else {
                for (AppId_t appId : TraceBetaList()) {
                    const std::string branch = SelectedBranchFromManifest(appId);
                    if (!branch.empty()) TryCallSetBetaBranch(appId, branch);
                }
            }
        }).detach();
    }

    bool InjectBranchManifests(AppId_t appId, const std::string& branch,
                               const std::vector<AppInfoFile::BranchDepot>& depots);

    // The branch the user selected, read from the appmanifest. The update job
    // reads the same key out of its own state KV, and it decides the beta
    // fallback before BuildDepotDependency runs, so the manifests have to be in
    // the appinfo KV as soon as that KV exists.

    // Steam writes these files while we read them; MSVC's ifstream sharing made
    // Steam's own write fail ("Access Denied" -> update canceled), so read with
    // full sharing instead.
    std::string ReadTextFileShared(const std::filesystem::path& path) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        std::string text;
        char buffer[8192];
        DWORD read = 0;
        while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            text.append(buffer, read);
        }
        CloseHandle(file);
        return text;
    }

    std::string SelectedBranchFromManifest(AppId_t appId) {
        const std::filesystem::path root = SteamRootDir();
        std::vector<std::filesystem::path> dirs{root / "steamapps"};
        const std::string libraryFolders = ReadTextFileShared(root / "steamapps" / "libraryfolders.vdf");
        if (!libraryFolders.empty()) {
            for (const auto& p : SteamConfig::AppManifestBeta::LibraryFolders(libraryFolders)) {
                dirs.emplace_back(std::filesystem::path(p) / "steamapps");
            }
        }
        for (const auto& dir : dirs) {
            const std::string text = ReadTextFileShared(dir / std::format("appmanifest_{}.acf", appId));
            if (text.empty()) continue;
            if (auto key = SteamConfig::AppManifestBeta::FindBetaKey(text, "UserConfig"); key && !key->empty())
                return *key;
            if (auto key = SteamConfig::AppManifestBeta::FindBetaKey(text, "MountedConfig"); key && !key->empty())
                return *key;
        }
        return {};
    }

    void InjectForKnownApp(AppId_t appId) {
        if (!Config::GetManifestInjectBranchManifests() && !TraceBeta(appId)) return;
        const std::string branch = SelectedBranchFromManifest(appId);
        if (branch.empty() || _stricmp(branch.c_str(), "public") == 0) return;
        if (_stricmp(branch.c_str(), "Pinned") == 0) {
            const std::string source = PinnedSourceFor(appId);
            if (!source.empty() && InjectPinnedBranch(appId, source)) {
                EnsureBetahashValue(appId, "Pinned");
                LOG_MANIFEST_INFO("BETA-PIN: app {} pinned to branch \"{}\"", appId, source);
            }
            return;
        }
        EnsureBetahashValue(appId, branch);
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
        // Inject immediately: the client resolves the selected branch while it is
        // still loading this very node, so a delayed injection is always too late.
        InjectForKnownApp(appId);
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

    // ── synthetic "Pinned" branch, the way the reference tool does it ────
    // branches/Pinned mirrors the selected beta (buildid, description, no
    // password and no local-content-server requirement) and every depot gets
    // depots/<depot>/manifests/Pinned with that beta's real gid/size. The client
    // then treats Pinned as an ordinary branch that the account is subscribed to
    // (betahash_Pinned), so its own config change / scheduler / download path
    // runs instead of the public fallback.
    std::vector<std::pair<AppId_t, std::string>> PinnedSources() {
        std::vector<std::pair<AppId_t, std::string>> out;
        std::ifstream in(SteamRootDir() / "opensteamtool" / "pin.txt");
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const AppId_t appId = static_cast<AppId_t>(std::strtoul(line.substr(0, eq).c_str(), nullptr, 10));
            const std::string branch = line.substr(eq + 1);
            if (appId && !branch.empty()) out.emplace_back(appId, branch);
        }
        return out;
    }

    std::string PinnedSourceFor(AppId_t appId) {
        for (const auto& [id, branch] : PinnedSources()) {
            if (id == appId) return branch;
        }
        return {};
    }

    bool InjectPinnedBranch(AppId_t appId, const std::string& sourceBranch) {
        void* appKv = FindAppKv(appId);
        if (!appKv || !oKvFindFmt) return false;
        {
            std::lock_guard<std::mutex> lock(g_appKvMutex);
            if (!g_injected.insert(std::format("{}|Pinned", appId)).second) return true;
        }
        AppInfoFile::Refresh(SteamRootDir().string());
        const auto depots = AppInfoFile::Lookup(appId, sourceBranch);
        const auto buildId = AppInfoFile::LookupBuildId(appId, sourceBranch);
        if (depots.empty()) return false;
        fnKvSetString = reinterpret_cast<KvSetString_t>(
            reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll")) + 0xDA0290);
        fnKvSetInt = reinterpret_cast<KvSetInt_t>(
            reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll")) + 0xDA0250);
        if (!fnKvSetString || !fnKvSetInt) return false;

        void* branchNode = oKvFindFmt(appKv, "branches/Pinned", true, nullptr, nullptr, nullptr);
        if (!branchNode) return false;
        auto setStr = [](void* parent, const char* key, const char* value) {
            void* node = oKvFindFmt(parent, key, true, nullptr, nullptr, nullptr);
            if (node) fnKvSetString(node, value);
        };
        auto setInt = [](void* parent, const char* key, int value) {
            void* node = oKvFindFmt(parent, key, true, nullptr, nullptr, nullptr);
            if (node) fnKvSetInt(node, value);
        };
        setStr(branchNode, "description", sourceBranch.c_str());
        setInt(branchNode, "buildid", static_cast<int>(buildId));
        setInt(branchNode, "timeupdated", static_cast<int>(std::time(nullptr)));
        // The reference tool writes plain zeroes here (xor edx,edx + SetInt).
        setInt(branchNode, "pwdrequired", 0);
        setInt(branchNode, "lcsrequired", 0);
        LOG_MANIFEST_INFO("BETA-PIN: app {} branches/Pinned <- branch \"{}\" build={}",
                          appId, sourceBranch, buildId);

        bool any = false;
        // The live KV keeps depot nodes as direct children of the app node
        // ("<depot>/manifests/<branch>/..."), while the on-disk appinfo nests them
        // under "depots/". Write both so whichever one the client walks finds the
        // beta's gid/size.
        for (const auto& depot : depots) {
            if (!depot.depotId) continue;
            char gidBuf[32] = {};
            char sizeBuf[32] = {};
            std::snprintf(gidBuf, sizeof(gidBuf), "%llu", static_cast<unsigned long long>(depot.gid));
            std::snprintf(sizeBuf, sizeof(sizeBuf), "%llu", static_cast<unsigned long long>(depot.size));
            for (const char* prefix : {"", "depots/"}) {
                const std::string base = std::format("{}{}/manifests/Pinned", prefix, depot.depotId);
                void* gidNode = oKvFindFmt(appKv, (base + "/gid").c_str(), true, nullptr, nullptr, nullptr);
                void* sizeNode = oKvFindFmt(appKv, (base + "/size").c_str(), true, nullptr, nullptr, nullptr);
                if (gidNode) {
                    fnKvSetString(gidNode, gidBuf);
                    any = true;
                }
                if (sizeNode) fnKvSetString(sizeNode, sizeBuf);
            }
            LOG_MANIFEST_INFO("BETA-PIN: app {} depot {} manifests/Pinned gid={} size={}",
                              appId, depot.depotId, depot.gid, depot.size);
        }
        return any;
    }

    // The client populates an unknown branch by copying the public manifest table
    // (same behaviour as the reference tool, which copies manifests/public into
    // manifests/Pinned). Swap those few values for the pinned branch's real ones
    // just before the switch, then put the public values back.
    bool SwapPublicManifestTable(AppId_t appId, const std::string& sourceBranch, bool install) {
        void* appKv = FindAppKv(appId);
        if (!appKv || !oKvFindFmt) return false;
        AppInfoFile::Refresh(SteamRootDir().string());
        const auto from = AppInfoFile::Lookup(appId, install ? "public" : sourceBranch);
        const auto to = AppInfoFile::Lookup(appId, install ? sourceBranch : "public");
        if (from.empty() || to.empty()) return false;
        fnKvSetString = reinterpret_cast<KvSetString_t>(
            reinterpret_cast<uint8_t*>(GetModuleHandleW(L"steamclient64.dll")) + 0xDA0290);
        if (!fnKvSetString) return false;
        bool any = false;
        for (const auto& src : from) {
            const auto it = std::find_if(to.begin(), to.end(),
                [&](const AppInfoFile::BranchDepot& d) { return d.depotId == src.depotId; });
            if (it == to.end()) continue;
            for (int which = 0; which < 2; ++which) {
                const char* key = which == 0 ? "gid" : "size";
                const uint64_t value = which == 0 ? it->gid : it->size;
                for (const char* prefix : {"", "depots/"}) {
                    const std::string path = std::format("{}{}/manifests/public/{}", prefix, src.depotId, key);
                    void* node = oKvFindFmt(appKv, path.c_str(), true, nullptr, nullptr, nullptr);
                    if (!node) continue;
                    char buf[32] = {};
                    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
                    fnKvSetString(node, buf);
                    any = true;
                }
            }
            LOG_MANIFEST_INFO("BETA-SWAP: app {} depot {} manifests/public <- {} {}",
                              appId, src.depotId,
                              install ? sourceBranch : "public", install ? "(pinned values)" : "(restored)");
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
        if (fmt && !create && parent && std::strncmp(fmt, "branches/", 9) == 0 &&
            (Config::GetManifestInjectBranchManifests() || !TraceBetaList().empty())) {
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
        if (create && result) {
            RememberAppKv(fmt, result);
            uint64_t depotId = 0;
            bool isSize = false;
            if (Config::GetManifestInjectBranchManifests() && ParsePinnedManifestPath(fmt, depotId, isSize)) {
                const AppId_t owner = AppIdForKv(parent);
                if (owner != 0) {
                    std::lock_guard<std::mutex> lock(g_pinnedNodesMutex);
                    g_pinnedNodes[result] = PinnedNodeInfo{owner, depotId, isSize};
                }
            }
        }
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
        if (!KvTraceWanted() && !Config::GetManifestInjectBranchManifests()) return;
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
        // With the appinfo injection active the client resolves the branch on its
        // own, and rewriting the depot list here would stamp the branch gids as
        // "installed" (the exact corruption this replaces). The synthetic branch
        // is the exception: the client clones the public table for it, so the
        // answer has to come from here.
        std::string answerBranch;
        const std::string betakey = BranchFromUserConfig(reinterpret_cast<KeyValues*>(pUserConfig));
        if (TraceBeta(AppId)) {
            LOG_MANIFEST_INFO("BETA-CALL: app {} betakey=\"{}\" fallback={} depots={} build={}",
                              AppId, betakey,
                              (pbBetaFallback && *pbBetaFallback) ? 1 : 0,
                              pDepotInfo ? static_cast<int>(pDepotInfo->m_Size) : -1,
                              (pBuildId ? static_cast<long long>(*pBuildId) : -1LL));
        }
        if (_stricmp(betakey.c_str(), "Pinned") == 0) {
            answerBranch = PinnedSourceFor(AppId);
        } else if (pbBetaFallback && *pbBetaFallback && !Config::GetManifestInjectBranchManifests()) {
            answerBranch = betakey;
        }
        if (!answerBranch.empty() && AppId != 0 && Config::GetManifestAnswerBetaFallback() &&
            _stricmp(answerBranch.c_str(), "public") != 0) {
            const std::string& branch = answerBranch;
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
                    if (pbBetaFallback) *pbBetaFallback = false;
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
        InstallKvSetTrace();
        InstallKvSetUint64Hook();
        InstallBetaAccessGrant();
        InstallBetaHashReadGrant();
        InstallBetaTokenGrant();
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
