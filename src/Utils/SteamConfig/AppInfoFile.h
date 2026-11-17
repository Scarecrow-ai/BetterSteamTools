#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Reads the client's appinfo cache (<Steam>/appcache/appinfo.vdf) and answers
// "which depot manifests does branch X of app Y need?".
//
// That file is the only authoritative local source for a branch's build: the
// product-info replies Steam sends at startup are metadata-only once the cache is
// warm, and the client itself refuses to use the branch manifests it has stored
// here for an app it believes it does not own (it falls back to the public branch
// and rolls the app back).
//
// Layout, established from the live file:
//   header: magic u32 (0x07564429) | universe u32 | stringTableOffset u64
//   string pool at stringTableOffset: count u32, then count NUL-terminated strings
//   appinfo blobs: binary KeyValues where keys are indices into that pool and
//   string values are stored inline as NUL-terminated bytes
//     appinfo { appid <n> depots { "<depot>" { manifests { "<branch>" {
//         gid "<u64>" size "<u64>" } } } } }
// The blobs are located by scanning for the nested root and validated strictly,
// so the per-entry record header (which is not needed) never has to be parsed.
namespace AppInfoFile {

    struct BranchDepot {
        uint32_t depotId = 0;
        uint64_t gid     = 0;
        uint64_t size    = 0;
    };

    // Re-reads the cache when the file changed. Cheap to call often; the parse is
    // skipped unless (size, mtime) differ from the last successful read.
    bool Refresh(const std::string& steamRoot);

    // Branch depot list for one app, or empty when the cache has no such entry.
    std::vector<BranchDepot> Lookup(uint32_t appId, std::string_view branch);

    struct Stats {
        bool     loaded     = false;
        uint32_t apps       = 0;   // apps with at least one branch manifest
        uint32_t branches   = 0;   // distinct branch entries
        uint32_t manifests  = 0;   // depot manifest rows
        uint32_t candidates = 0;   // root candidates tried
    };
    Stats GetStats();
}
