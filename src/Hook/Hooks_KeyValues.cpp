// Hook KeyValues::ReadAsBinary — entry point for KV-tree reads.
//
// Used to harvest the per-branch depot manifests out of the appinfo blob the
// client decodes: Steam's product-info reply for an app carries
//
//     depots { "<depotId>" { manifests { "<branch>" { gid "<u64>" size "<u64>" } } } }
//
// and for an app the client believes it does not own it then refuses to use
// those entries when it builds the depot config, falls back to the public
// branch and rolls the app back. Harvesting the table here gives BetaKeeper the
// authoritative branch -> depot manifest mapping, so that fallback can be
// answered with the branch's real build instead of a guess.
//
// Depot patching itself still lives in Hooks_Manifest::BuildDepotDependency.

#include "Hooks_KeyValues.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "Utils/SteamConfig/BetaKeeper.h"

#include <cstdlib>

namespace {

    // Pattern names are matched exactly (FNV-1a of the name in the TOML), and the
    // steamclient pattern set spells them "KeyValues_<Method>", so the hook
    // symbols carry the same names — "ReadAsBinary" alone never resolved, which is
    // why this file's hook was commented out.
    RESOLVE_FUNC(KeyValues_FindOrCreateKey, KeyValues*, KeyValues* parent, const char* name, bool create, KeyValues** out);
    KeyValues* KV_FindKey(KeyValues* parent, const char* name) {
        return oKeyValues_FindOrCreateKey ? oKeyValues_FindOrCreateKey(parent, name, false, nullptr) : nullptr;
    }

    // ── KeyValuesSystem — symbol ↔ string (from vstdlib_s64.dll) ───
    IKeyValuesSystem* GetKeyValuesSystem() {
        static IKeyValuesSystem* sys = []() -> IKeyValuesSystem* {
            auto vstdlib = OSTPlatform::DynamicLibrary::GetLoaded("vstdlib_s64.dll");
            if (!vstdlib) return nullptr;
            auto pfn = reinterpret_cast<KeyValuesSystemSteam_t>(
                OSTPlatform::DynamicLibrary::GetSymbol(vstdlib, "KeyValuesSystemSteam"));
            return pfn ? pfn() : nullptr;
        }();
        return sys;
    }

    const char* GetKeyName(int symbol) {
        auto* sys = GetKeyValuesSystem();
        if (!sys) return nullptr;
        const char* name = sys->GetStringForSymbol(symbol);
        return name && *name ? name : nullptr;
    }

    // ── tree helpers ────────────────────────────────────────────────
    KeyValues* Child(KeyValues* parent, const char* name) {
        auto* sys = GetKeyValuesSystem();
        if (!sys || !parent) return nullptr;
        const int want = sys->GetSymbol(name);
        if (!want) return nullptr;
        for (KeyValues* child = parent->m_pSub; child; child = child->m_pPeer) {
            if (static_cast<int>(child->m_iKeyName) == want) return child;
        }
        return nullptr;
    }

    uint64_t AsU64(KeyValues* kv) {
        if (!kv) return 0;
        switch (kv->m_iDataType) {
            case KV_TYPE_UINT64: return kv->m_ullValue;
            case KV_TYPE_INT64:  return static_cast<uint64_t>(kv->m_llValue);
            case KV_TYPE_INT:    return static_cast<uint64_t>(static_cast<uint32_t>(kv->m_iValue));
            case KV_TYPE_STRING: return kv->m_sValue ? ::strtoull(kv->m_sValue, nullptr, 10) : 0;
            default:             return 0;
        }
    }

    // Reads depots/<depot>/manifests/<branch>/{gid,size} for one appinfo tree.
    void HarvestAppInfo(KeyValues* root) {
        if (!root || !GetKeyValuesSystem()) return;
        if (!Child(root, "depots")) return;              // not an appinfo blob

        const AppId_t appId = static_cast<AppId_t>(AsU64(Child(root, "appid")));
        if (!appId || !BetaKeeper::IsTracked(appId)) return;

        KeyValues* depots = Child(root, "depots");
        uint32_t harvested = 0;
        for (KeyValues* depot = depots->m_pSub; depot; depot = depot->m_pPeer) {
            const char* depotKey = GetKeyName(static_cast<int>(depot->m_iKeyName));
            if (!depotKey) continue;
            const AppId_t depotId = static_cast<AppId_t>(::strtoul(depotKey, nullptr, 10));
            if (!depotId) continue;

            KeyValues* manifests = Child(depot, "manifests");
            if (!manifests) continue;

            for (KeyValues* branch = manifests->m_pSub; branch; branch = branch->m_pPeer) {
                const char* branchName = GetKeyName(static_cast<int>(branch->m_iKeyName));
                if (!branchName) continue;

                const uint64_t gid  = AsU64(Child(branch, "gid"));
                const uint64_t size = AsU64(Child(branch, "size"));
                if (!gid) continue;

                BetaKeeper::NoteBranchManifest(appId, branchName, depotId, gid, size);
                ++harvested;
            }
        }

        if (harvested) {
            LOG_KEYVALUE_DEBUG("appinfo {}: harvested {} branch manifest entr(ies)", appId, harvested);
        }
    }

    HOOK_FUNC(KeyValues_ReadAsBinary, bool, KeyValues* root, void* buf, int depth,
              bool textMode, void* symTable) {
        bool ok = oKeyValues_ReadAsBinary(root, buf, depth, textMode, symTable);
        if (ok) HarvestAppInfo(root);
        return ok;
    }

} // anonymous namespace

namespace Hooks_KeyValues {

    void Install() {
        RESOLVE_C(KeyValues_FindOrCreateKey);

        HOOK_BEGIN();
        INSTALL_HOOK_C(KeyValues_ReadAsBinary);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(KeyValues_ReadAsBinary);
        UNHOOK_END();
    }

} // namespace Hooks_KeyValues
