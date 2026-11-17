#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Reads the per-branch depot manifests out of a CMsgClientPICSProductInfoResponse
// body. Steam serves the appinfo blob as binary KeyValues, keyed by indices into
// the message's string table:
//
//   appinfo { appid <u32> depots { "<depot>" { manifests { "<branch>" {
//       gid <u64> size <u64> } } } } }
//
// That table is the authoritative branch -> build mapping, and it is what
// steamclient itself refuses to use for an app it believes it does not own (it
// falls back to the public branch instead). Harvesting it here lets a beta
// fallback be answered with the branch's real build.
//
// The parser is deliberately self-validating and tolerant of field-number drift:
// it walks the message generically, recognises the string-table field by
// content, and only accepts an appinfo whose KV parses exactly to the end of its
// buffer with the root key "appinfo".
namespace PicsAppInfo {

    struct BranchManifest {
        uint32_t depotId = 0;
        uint64_t gid     = 0;
        uint64_t size    = 0;
    };

    struct BranchEntry {
        std::string  branch;
        BranchManifest manifest;
    };

    struct HarvestResult {
        uint32_t apps    = 0;   // appinfo buffers that parsed
        uint32_t entries = 0;   // branch manifest entries handed to the visitor
        uint32_t blobs   = 0;   // binary-KV-looking buffers found in AppInfo fields
        uint32_t rejected = 0;  // buffers that did not parse as a clean appinfo
        bool     stringTable = false;
    };

    // `visit` is called once per (app, branch, depot) triple.
    HarvestResult Harvest(const uint8_t* body, size_t size,
                          const std::function<void(uint32_t appId, const BranchEntry&)>& visit);

    // Parses one appinfo blob against a string table. Exposed for tests.
    bool ParseAppInfo(std::string_view blob, const std::vector<std::string>& stringTable,
                      uint32_t& outAppId, std::vector<BranchEntry>& outEntries);

    // Parses "repeated string" field content (tag 1, wire 2).
    bool ParseStringList(std::string_view content, std::vector<std::string>& out);
}
