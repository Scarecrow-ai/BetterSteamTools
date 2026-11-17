#include "AppInfoFile.h"

#include "dllmain.h"                          // log macros
#include "OSTPlatform/include/Encoding.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace {

    using BranchDepot = AppInfoFile::BranchDepot;

    struct AppEntry {
        std::unordered_map<std::string, std::vector<BranchDepot>> branches;
    };

    struct State {
        std::mutex mutex;
        std::string path;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type mtime{};
        bool loaded = false;
        AppInfoFile::Stats stats;
        std::unordered_map<uint32_t, AppEntry> apps;
    };

    State& Store() {
        static State state;
        return state;
    }

    // ── primitive readers over a byte range ─────────────────────────────────
    struct Cursor {
        const uint8_t* p   = nullptr;
        const uint8_t* end = nullptr;

        size_t left() const { return static_cast<size_t>(end - p); }
        bool ok() const { return p && p <= end; }

        bool U8(uint8_t& out) {
            if (left() < 1) return false;
            out = *p++;
            return true;
        }
        bool U32(uint32_t& out) {
            if (left() < 4) return false;
            out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                  (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            p += 4;
            return true;
        }
        bool U64(uint64_t& out) {
            uint32_t lo = 0, hi = 0;
            if (!U32(lo) || !U32(hi)) return false;
            out = (static_cast<uint64_t>(hi) << 32) | lo;
            return true;
        }
        // NUL-terminated inline string (the cache stores values this way).
        bool CStr(std::string_view& out, size_t limit = 512) {
            const void* nul = std::memchr(p, 0, std::min(limit, left()));
            if (!nul) return false;
            const size_t len = static_cast<size_t>(static_cast<const uint8_t*>(nul) - p);
            out = std::string_view(reinterpret_cast<const char*>(p), len);
            p += len + 1;
            return true;
        }
    };

    bool ParseU64(std::string_view text, uint64_t& out) {
        if (text.empty()) return false;
        uint64_t value = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') return false;
            value = value * 10 + static_cast<uint64_t>(ch - '0');
        }
        out = value;
        return true;
    }

    void JoinPath(const std::vector<std::string>& path, std::string_view leaf, std::string& out) {
        out.clear();
        for (const auto& part : path) {
            out += part;
            out += '/';
        }
        out.append(leaf.data(), leaf.size());
    }

    // ── binary KeyValues, keys by pool index, strings inline ────────────────
    bool WalkKv(const std::vector<std::string>& pool, Cursor& r, int depth,
                std::vector<std::string>& path,
                std::vector<std::pair<std::string, std::string>>& leaves) {
        if (depth > 8) return false;
        std::string full;
        while (true) {
            uint8_t type = 0;
            if (!r.U8(type)) return false;
            if (type == 8) return true;                       // end of this level

            uint32_t keyIndex = 0;
            if (!r.U32(keyIndex) || keyIndex >= pool.size()) return false;
            const std::string& name = pool[keyIndex];

            switch (type) {
                case 0:
                    path.push_back(name);
                    if (!WalkKv(pool, r, depth + 1, path, leaves)) return false;
                    path.pop_back();
                    break;
                case 1:
                case 5: {
                    std::string_view value;
                    if (!r.CStr(value)) return false;
                    JoinPath(path, name, full);
                    leaves.emplace_back(full, std::string(value));
                    break;
                }
                case 2:
                case 3:
                case 4:
                case 6: {
                    uint32_t value = 0;
                    if (!r.U32(value)) return false;
                    JoinPath(path, name, full);
                    leaves.emplace_back(full, std::to_string(value));
                    break;
                }
                case 7:
                case 9: {
                    uint64_t value = 0;
                    if (!r.U64(value)) return false;
                    JoinPath(path, name, full);
                    leaves.emplace_back(full, std::to_string(value));
                    break;
                }
                default:
                    return false;
            }
        }
    }

    // Parses one appinfo blob at `offset`; on success fills `outAppId` and branches.
    bool ParseBlob(const std::string& data, size_t offset,
                   const std::vector<std::string>& pool, uint32_t appInfoKey,
                   uint32_t& outAppId, AppEntry& out) {
        Cursor r{reinterpret_cast<const uint8_t*>(data.data()) + offset,
                 reinterpret_cast<const uint8_t*>(data.data()) + data.size()};

        uint8_t rootType = 0;
        uint32_t rootKey = 0;
        if (!r.U8(rootType) || rootType != 0) return false;
        if (!r.U32(rootKey) || rootKey != appInfoKey) return false;

        std::vector<std::string> path{"appinfo"};
        std::vector<std::pair<std::string, std::string>> leaves;
        if (!WalkKv(pool, r, 1, path, leaves)) return false;

        uint8_t terminator = 0;
        if (!r.U8(terminator) || terminator != 8) return false;   // top-level end

        uint32_t appId = 0;
        for (const auto& [key, value] : leaves) {
            if (key != "appinfo/appid") continue;
            uint64_t v = 0;
            if (ParseU64(value, v)) appId = static_cast<uint32_t>(v);
            break;
        }
        if (!appId) return false;

        // appinfo/depots/<depot>/manifests/<branch>/{gid,size}
        constexpr std::string_view kPrefix = "appinfo/depots/";
        struct Slot {
            uint32_t depotId = 0;
            uint64_t gid   = 0;
            uint64_t size  = 0;
            bool hasGid    = false;
        };
        std::unordered_map<std::string, Slot> slots;   // key: "<branch>/<depot>"

        for (const auto& [key, value] : leaves) {
            if (key.size() <= kPrefix.size() || key.compare(0, kPrefix.size(), kPrefix) != 0) {
                continue;
            }
            const std::string_view rest(key.data() + kPrefix.size(), key.size() - kPrefix.size());
            const size_t s1 = rest.find('/');
            if (s1 == std::string_view::npos) continue;
            const size_t s2 = rest.find('/', s1 + 1);
            if (s2 == std::string_view::npos) continue;
            if (rest.substr(s1 + 1, s2 - s1 - 1) != "manifests") continue;
            const size_t s3 = rest.find('/', s2 + 1);
            if (s3 == std::string_view::npos) continue;

            uint64_t depotId = 0;
            if (!ParseU64(rest.substr(0, s1), depotId) || depotId == 0 ||
                depotId > 0xFFFFFFFFull) {
                continue;
            }
            const std::string_view branch = rest.substr(s2 + 1, s3 - s2 - 1);
            const std::string_view field  = rest.substr(s3 + 1);
            if (branch.empty()) continue;

            uint64_t v = 0;
            if (!ParseU64(value, v)) continue;

            std::string slotKey;
            slotKey.reserve(branch.size() + 12);
            slotKey.append(branch.data(), branch.size());
            slotKey += '|';
            slotKey += std::to_string(depotId);

            Slot& slot = slots[slotKey];
            slot.depotId = static_cast<uint32_t>(depotId);
            if (field == "gid") {
                slot.gid = v;
                slot.hasGid = true;
            } else if (field == "size") {
                slot.size = v;
            }
        }

        for (const auto& [slotKey, slot] : slots) {
            if (!slot.hasGid || slot.gid == 0) continue;
            const size_t bar = slotKey.find('|');
            if (bar == std::string::npos) continue;
            const std::string branch = slotKey.substr(0, bar);
            BranchDepot depot;
            depot.depotId = slot.depotId;
            depot.gid = slot.gid;
            depot.size = slot.size;
            out.branches[branch].push_back(depot);
        }
        if (out.branches.empty()) return false;

        outAppId = appId;
        return true;
    }

    std::optional<std::string> ReadFile(const std::string& path, std::uintmax_t& outSize) {
        std::error_code ec;
        outSize = std::filesystem::file_size(path, ec);
        if (ec || outSize == 0 || outSize > (64ull << 20)) return std::nullopt;

        std::ifstream in(OSTPlatform::Encoding::Utf8ToWide(path), std::ios::binary);
        if (!in) return std::nullopt;

        std::string data(static_cast<size_t>(outSize), '\0');
        in.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (in.gcount() != static_cast<std::streamsize>(data.size())) return std::nullopt;
        return data;
    }

} // namespace

namespace AppInfoFile {

    bool Refresh(const std::string& steamRoot) {
        State& state = Store();

        const std::string path = steamRoot + "\\appcache\\appinfo.vdf";
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(
            std::filesystem::path(OSTPlatform::Encoding::Utf8ToWide(path)), ec);

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.loaded && !ec && state.path == path && state.mtime == mtime) return true;
        }

        std::uintmax_t size = 0;
        const auto file = ReadFile(path, size);
        if (!file) {
            LOG_BETA_DEBUG("appinfo cache: cannot read {}", path);
            return false;
        }
        const std::string& data = *file;

        // header
        if (data.size() < 20) return false;
        Cursor head{reinterpret_cast<const uint8_t*>(data.data()),
                    reinterpret_cast<const uint8_t*>(data.data()) + data.size()};
        uint32_t magic = 0, universe = 0;
        uint64_t poolOffset = 0;
        if (!head.U32(magic) || !head.U32(universe) || !head.U64(poolOffset)) return false;
        if (magic != 0x07564429 || poolOffset == 0 || poolOffset >= data.size()) {
            LOG_BETA_DEBUG("appinfo cache: unexpected header (magic=0x{:08X})", magic);
            return false;
        }

        // string pool
        Cursor poolCursor{reinterpret_cast<const uint8_t*>(data.data()) + poolOffset,
                          reinterpret_cast<const uint8_t*>(data.data()) + data.size()};
        uint32_t count = 0;
        if (!poolCursor.U32(count) || count == 0 || count > 200000) return false;

        std::vector<std::string> pool;
        pool.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::string_view item;
            if (!poolCursor.CStr(item, 4096)) return false;
            pool.emplace_back(item);
        }

        uint32_t appInfoKey = UINT32_MAX;
        for (uint32_t i = 0; i < pool.size(); ++i) {
            if (pool[i] == "appinfo") {
                appInfoKey = i;
                break;
            }
        }
        if (appInfoKey == UINT32_MAX) {
            LOG_BETA_DEBUG("appinfo cache: no \"appinfo\" key in the string pool");
            return false;
        }

        // Scan for appinfo blobs. The per-entry record header is not needed: the
        // root tag is found and the blob is then validated by the strict walk.
        Stats stats;
        stats.loaded = true;
        std::unordered_map<uint32_t, AppEntry> apps;

        const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
        const size_t limit = data.size() >= 64 ? data.size() - 64 : 0;
        for (size_t i = 16; i + 6 <= limit; ++i) {
            if (bytes[i] != 0) continue;
            const uint32_t keyIndex = static_cast<uint32_t>(bytes[i + 1]) |
                                      (static_cast<uint32_t>(bytes[i + 2]) << 8) |
                                      (static_cast<uint32_t>(bytes[i + 3]) << 16) |
                                      (static_cast<uint32_t>(bytes[i + 4]) << 24);
            if (keyIndex != appInfoKey) continue;
            ++stats.candidates;

            uint32_t appId = 0;
            AppEntry entry;
            if (!ParseBlob(data, i, pool, appInfoKey, appId, entry)) continue;
            if (apps.count(appId)) continue;             // first occurrence wins

            ++stats.apps;
            stats.branches += static_cast<uint32_t>(entry.branches.size());
            for (const auto& [branch, depots] : entry.branches) {
                (void)branch;
                stats.manifests += static_cast<uint32_t>(depots.size());
            }
            apps.emplace(appId, std::move(entry));
        }

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.path = path;
            state.size = size;
            state.mtime = ec ? std::filesystem::file_time_type{} : mtime;
            state.loaded = true;
            state.stats = stats;
            state.apps = std::move(apps);
        }

        LOG_BETA_INFO("appinfo cache: {} app(s), {} branch(es), {} manifest row(s) "
                      "from {} candidate offset(s)", stats.apps, stats.branches,
                      stats.manifests, stats.candidates);
        return true;
    }

    std::vector<BranchDepot> Lookup(uint32_t appId, std::string_view branch) {
        std::lock_guard<std::mutex> lock(Store().mutex);
        const auto app = Store().apps.find(appId);
        if (app == Store().apps.end()) return {};
        const auto entry = app->second.branches.find(std::string(branch));
        if (entry == app->second.branches.end()) return {};
        return entry->second;
    }

    Stats GetStats() {
        std::lock_guard<std::mutex> lock(Store().mutex);
        return Store().stats;
    }

} // namespace AppInfoFile
