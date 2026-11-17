#include "PicsAppInfo.h"

#include <cstring>

namespace PicsAppInfo {
namespace {

    // ── protobuf wire reader (varints) ────────────────────────────────────────
    struct PbReader {
        const uint8_t* p   = nullptr;
        const uint8_t* end = nullptr;

        size_t left() const { return static_cast<size_t>(end - p); }

        bool Varint(uint64_t& out) {
            out = 0;
            int shift = 0;
            while (p < end) {
                const uint8_t byte = *p++;
                out |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if ((byte & 0x80) == 0) return true;
                shift += 7;
                if (shift > 63) return false;
            }
            return false;
        }

        bool Tag(uint32_t& field, uint32_t& wire) {
            uint64_t tag = 0;
            if (!Varint(tag) || tag == 0) return false;
            field = static_cast<uint32_t>(tag >> 3);
            wire  = static_cast<uint32_t>(tag & 0x7);
            return true;
        }

        bool Bytes(std::string_view& out) {
            uint64_t len = 0;
            if (!Varint(len) || len > left()) return false;
            out = std::string_view(reinterpret_cast<const char*>(p), static_cast<size_t>(len));
            p += len;
            return true;
        }

        bool Skip(uint32_t wire) {
            uint64_t ignored = 0;
            std::string_view skipped;
            switch (wire) {
                case 0: return Varint(ignored);
                case 1: if (left() < 8) return false; p += 8; return true;
                case 2: return Bytes(skipped);
                case 5: if (left() < 4) return false; p += 4; return true;
                default: return false;
            }
        }
    };

    // ── binary KeyValues reader (fixed-width little-endian) ───────────────────
    // type u8 | keyIndex u32 | value:
    //   0 nested, 1 string(table index u32), 2 int32, 3 float, 4 ptr,
    //   5 wstring(table index u32), 6 color, 7 uint64, 8 end, 9 int64.
    struct KvReader {
        const uint8_t* p   = nullptr;
        const uint8_t* end = nullptr;

        size_t left() const { return static_cast<size_t>(end - p); }

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
    };

    std::string JoinPath(const std::vector<std::string>& path, std::string_view leaf) {
        std::string full;
        size_t total = leaf.size();
        for (const auto& part : path) total += part.size() + 1;
        full.reserve(total);
        for (const auto& part : path) {
            full += part;
            full += '/';
        }
        full.append(leaf.data(), leaf.size());
        return full;
    }

    bool WalkKv(const std::vector<std::string>& table, KvReader& r, int depth,
                std::vector<std::string>& path,
                std::vector<std::pair<std::string, std::string>>& leaves) {
        if (depth > 8) return false;
        while (true) {
            uint8_t type = 0;
            if (!r.U8(type)) return false;
            if (type == 8) return true;                 // end of this level

            uint32_t keyIndex = 0;
            if (!r.U32(keyIndex) || keyIndex >= table.size()) return false;
            const std::string& name = table[keyIndex];

            switch (type) {
                case 0:
                    path.push_back(name);
                    if (!WalkKv(table, r, depth + 1, path, leaves)) return false;
                    path.pop_back();
                    break;
                case 1:
                case 5: {
                    uint32_t valueIndex = 0;
                    if (!r.U32(valueIndex) || valueIndex >= table.size()) return false;
                    leaves.emplace_back(JoinPath(path, name), table[valueIndex]);
                    break;
                }
                case 2:
                case 3:
                case 4:
                case 6: {
                    uint32_t value = 0;
                    if (!r.U32(value)) return false;
                    leaves.emplace_back(JoinPath(path, name), std::to_string(value));
                    break;
                }
                case 7:
                case 9: {
                    uint64_t value = 0;
                    if (!r.U64(value)) return false;
                    leaves.emplace_back(JoinPath(path, name), std::to_string(value));
                    break;
                }
                default:
                    return false;
            }
        }
    }

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

} // namespace

bool ParseStringList(std::string_view content, std::vector<std::string>& out) {
    PbReader r{reinterpret_cast<const uint8_t*>(content.data()),
               reinterpret_cast<const uint8_t*>(content.data()) + content.size()};
    while (r.left() > 0) {
        uint32_t field = 0, wire = 0;
        if (!r.Tag(field, wire)) return false;
        if (wire == 2) {
            std::string_view item;
            if (!r.Bytes(item)) return false;
            if (field == 1) out.emplace_back(item);
        } else if (!r.Skip(wire)) {
            return false;
        }
    }
    return !out.empty();
}

bool ParseAppInfo(std::string_view blob, const std::vector<std::string>& stringTable,
                  uint32_t& outAppId, std::vector<BranchEntry>& outEntries) {
    outEntries.clear();
    outAppId = 0;
    if (blob.size() < 6 || stringTable.empty()) return false;

    KvReader r{reinterpret_cast<const uint8_t*>(blob.data()),
               reinterpret_cast<const uint8_t*>(blob.data()) + blob.size()};

    // The root has to be a single nested "appinfo".
    uint8_t rootType = 0;
    uint32_t rootKey = 0;
    if (!r.U8(rootType) || rootType != 0) return false;
    if (!r.U32(rootKey) || rootKey >= stringTable.size()) return false;
    if (stringTable[rootKey] != "appinfo") return false;

    std::vector<std::string> path;
    path.emplace_back(stringTable[rootKey]);            // "appinfo" prefixes every leaf
    std::vector<std::pair<std::string, std::string>> leaves;
    if (!WalkKv(stringTable, r, 1, path, leaves)) return false;

    // The blob is a single root-level pair: the root object's terminator was
    // consumed above, so exactly one more terminator must close the top level.
    uint8_t terminator = 0;
    if (!r.U8(terminator) || terminator != 8) return false;
    if (r.left() != 0) return false;                    // trailing bytes: not clean

    for (const auto& [key, value] : leaves) {
        if (key != "appinfo/appid") continue;
        uint64_t v = 0;
        if (ParseU64(value, v)) outAppId = static_cast<uint32_t>(v);
        break;
    }

    // appinfo/depots/<depot>/manifests/<branch>/{gid,size}
    constexpr std::string_view kPrefix = "appinfo/depots/";
    struct Pending {
        uint32_t depotId = 0;
        std::string branch;
        uint64_t gid   = 0;
        uint64_t size  = 0;
        bool hasGid    = false;
    };
    std::vector<Pending> pending;

    for (const auto& [key, value] : leaves) {
        if (key.size() <= kPrefix.size()) continue;
        if (key.compare(0, kPrefix.size(), kPrefix) != 0) continue;
        const std::string_view rest(key.data() + kPrefix.size(), key.size() - kPrefix.size());

        const size_t s1 = rest.find('/');
        if (s1 == std::string_view::npos) continue;
        const size_t s2 = rest.find('/', s1 + 1);
        if (s2 == std::string_view::npos) continue;
        if (rest.substr(s1 + 1, s2 - s1 - 1) != "manifests") continue;
        const size_t s3 = rest.find('/', s2 + 1);
        if (s3 == std::string_view::npos) continue;

        uint64_t depotId = 0;
        if (!ParseU64(rest.substr(0, s1), depotId) || depotId == 0 || depotId > 0xFFFFFFFFull) continue;

        const std::string_view branch = rest.substr(s2 + 1, s3 - s2 - 1);
        const std::string_view field  = rest.substr(s3 + 1);
        if (branch.empty()) continue;

        Pending* slot = nullptr;
        for (auto& candidate : pending) {
            if (candidate.depotId == static_cast<uint32_t>(depotId) && candidate.branch == branch) {
                slot = &candidate;
                break;
            }
        }
        if (!slot) {
            pending.push_back(Pending{static_cast<uint32_t>(depotId), std::string(branch), 0, 0, false});
            slot = &pending.back();
        }
        uint64_t v = 0;
        if (!ParseU64(value, v)) continue;
        if (field == "gid") {
            slot->gid = v;
            slot->hasGid = true;
        } else if (field == "size") {
            slot->size = v;
        }
    }

    for (const auto& p : pending) {
        if (!p.hasGid || p.gid == 0) continue;
        BranchEntry entry;
        entry.branch = p.branch;
        entry.manifest.depotId = p.depotId;
        entry.manifest.gid = p.gid;
        entry.manifest.size = p.size;
        outEntries.push_back(std::move(entry));
    }
    return !outEntries.empty();
}

HarvestResult Harvest(const uint8_t* body, size_t size,
                      const std::function<void(uint32_t, const BranchEntry&)>& visit) {
    HarvestResult result;
    if (!body || size < 8) return result;

    struct FieldRef {
        uint32_t number = 0;
        std::string_view bytes;
        uint64_t varint = 0;
        bool isBytes = false;
    };
    std::vector<FieldRef> fields;

    PbReader r{body, body + size};
    while (r.left() > 0) {
        uint32_t field = 0, wire = 0;
        if (!r.Tag(field, wire)) break;
        if (wire == 2) {
            std::string_view bytes;
            if (!r.Bytes(bytes)) break;
            fields.push_back(FieldRef{field, bytes, 0, true});
        } else if (wire == 0) {
            uint64_t v = 0;
            if (!r.Varint(v)) break;
            fields.push_back(FieldRef{field, {}, v, false});
        } else if (!r.Skip(wire)) {
            break;
        }
    }
    if (fields.empty()) return result;

    // String table: the length-delimited field that decodes as repeated strings
    // and mentions the appinfo keys. Content-based, so field numbers may drift.
    std::vector<std::string> table;
    int tableIndex = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (!fields[i].isBytes || fields[i].bytes.size() < 8) continue;
        std::vector<std::string> candidate;
        if (!ParseStringList(fields[i].bytes, candidate)) continue;
        bool hasAppInfo = false, hasDepots = false;
        for (const auto& s : candidate) {
            if (s == "appinfo") hasAppInfo = true;
            if (s == "depots") hasDepots = true;
        }
        if (!hasAppInfo || !hasDepots) continue;
        if (candidate.size() > table.size()) {
            table = std::move(candidate);
            tableIndex = static_cast<int>(i);
        }
    }
    if (tableIndex < 0) return result;
    result.stringTable = true;

    for (size_t i = 0; i < fields.size(); ++i) {
        if (static_cast<int>(i) == tableIndex || !fields[i].isBytes) continue;

        PbReader app{reinterpret_cast<const uint8_t*>(fields[i].bytes.data()),
                     reinterpret_cast<const uint8_t*>(fields[i].bytes.data()) + fields[i].bytes.size()};
        uint32_t messageAppId = 0;
        std::string_view buffer;
        bool sawBuffer = false;
        while (app.left() > 0) {
            uint32_t field = 0, wire = 0;
            if (!app.Tag(field, wire)) break;
            if (wire == 0) {
                uint64_t v = 0;
                if (!app.Varint(v)) break;
                if (field == 1 && messageAppId == 0) messageAppId = static_cast<uint32_t>(v);
            } else if (wire == 2) {
                std::string_view bytes;
                if (!app.Bytes(bytes)) break;
                if (bytes.size() > 16 && static_cast<uint8_t>(bytes[0]) == 0) {
                    buffer = bytes;        // binary KV begins with a nested type
                    sawBuffer = true;
                }
            } else if (!app.Skip(wire)) {
                break;
            }
        }
        if (!sawBuffer) continue;
        ++result.blobs;

        uint32_t appId = 0;
        std::vector<BranchEntry> entries;
        if (!ParseAppInfo(buffer, table, appId, entries)) {
            ++result.rejected;
            continue;
        }
        if (!appId) appId = messageAppId;
        if (!appId) continue;

        ++result.apps;
        for (const auto& entry : entries) {
            ++result.entries;
            if (visit) visit(appId, entry);
        }
    }
    return result;
}

} // namespace PicsAppInfo
