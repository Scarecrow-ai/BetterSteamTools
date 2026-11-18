#pragma once

// Text helpers for Steam's per-app manifest (steamapps/appmanifest_<appid>.acf).
//
// A game's selected test branch (Library -> Properties -> Betas) is persisted
// in that file, not in the client config:
//
//   "AppState"
//   {
//       "appid"      "570"
//       "UserConfig"
//       {
//           "betakey"       "openbeta"      <- the user's choice
//       }
//       "MountedConfig"
//       {
//           "betakey"       "openbeta"      <- branch the installed build came from
//       }
//   }
//
// When steamclient cannot resolve that branch it rewrites the manifest WITHOUT
// the key (the same silent "field is removed" behaviour reported for steamcmd
// in ValveSoftware/steam-for-linux#9076), which is why a selected branch looks
// like it "reverts to default" on the next launch. Keeping a copy of the value
// and putting it back is therefore all a repair needs to do.
//
// Everything below is plain text/std code with no Win32, logging or Steam
// dependency, so the tokenizing and splicing rules can be exercised on their
// own (see the note at the bottom of README "Game test branches").

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SteamConfig::AppManifestBeta {

    // Case-insensitive ASCII compare — KeyValues key lookups ignore case, so
    // "BetaKey", "betakey" and "BETAKEY" all address the same entry.
    inline bool EqualsNoCase(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    }

    // One VDF token. `begin`/`end` are byte offsets into the source text
    // (quotes included) so callers can splice a replacement in place.
    struct Token {
        enum class Kind { Open, Close, Value };
        Kind             kind  = Kind::Value;
        std::string_view text;          // unquoted text, Value tokens only
        size_t           begin = 0;
        size_t           end   = 0;
    };

    // Minimal tokenizer for Valve's KeyValues text format: braces and quoted
    // strings, everything else (whitespace, stray characters) skipped. `//`
    // line comments are skipped as well. Escaped characters inside quotes
    // (the doubled backslashes Steam writes into paths) are kept verbatim.
    inline std::vector<Token> Tokenize(std::string_view text) {
        std::vector<Token> tokens;
        size_t i = 0;
        while (i < text.size()) {
            const char c = text[i];
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                while (i < text.size() && text[i] != '\n') ++i;
                continue;
            }
            if (c == '"') {
                const size_t begin = i;
                ++i;
                while (i < text.size() && text[i] != '"') {
                    if (text[i] == '\\' && i + 1 < text.size()) ++i;   // keep "\x" together
                    ++i;
                }
                const size_t innerEnd = i < text.size() ? i : text.size();
                if (i < text.size()) ++i;                              // closing quote
                Token token;
                token.kind  = Token::Kind::Value;
                token.text  = text.substr(begin + 1,
                                          innerEnd > begin + 1 ? innerEnd - begin - 1 : 0);
                token.begin = begin;
                token.end   = i;
                tokens.push_back(token);
                continue;
            }
            if (c == '{' || c == '}') {
                Token token;
                token.kind  = (c == '{') ? Token::Kind::Open : Token::Kind::Close;
                token.begin = i;
                token.end   = i + 1;
                tokens.push_back(token);
                ++i;
                continue;
            }
            ++i;
        }
        return tokens;
    }

    // Undo the doubled backslashes of a VDF string ("C:\\Steam" -> "C:\Steam").
    inline std::string Unescape(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                out += value[i + 1];
                ++i;
                continue;
            }
            out += value[i];
        }
        return out;
    }

    // The app this manifest belongs to ("appid" at the top of AppState).
    inline std::optional<uint32_t> ParseAppId(std::string_view text) {
        const auto tokens = Tokenize(text);
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].kind != Token::Kind::Value) continue;
            if (!EqualsNoCase(tokens[i].text, "appid")) continue;
            if (tokens[i + 1].kind != Token::Kind::Value) continue;

            const std::string_view digits = tokens[i + 1].text;
            if (digits.empty() || digits.size() > 10) return std::nullopt;
            uint64_t value = 0;
            for (char ch : digits) {
                if (ch < '0' || ch > '9') return std::nullopt;
                value = value * 10 + static_cast<uint64_t>(ch - '0');
            }
            if (value > 0xFFFFFFFFull) return std::nullopt;
            return static_cast<uint32_t>(value);
        }
        return std::nullopt;
    }

    // Value of "betakey" directly inside `section` ("UserConfig" /
    // "MountedConfig"). An empty value and an absent key both return nullopt.
    inline std::optional<std::string> FindBetaKey(std::string_view text,
                                                 std::string_view section) {
        const auto tokens = Tokenize(text);
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].kind != Token::Kind::Value) continue;
            if (!EqualsNoCase(tokens[i].text, section)) continue;
            if (tokens[i + 1].kind != Token::Kind::Open) continue;

            int depth = 1;
            for (size_t j = i + 2; j < tokens.size(); ++j) {
                const Token& token = tokens[j];
                if (token.kind == Token::Kind::Open) { ++depth; continue; }
                if (token.kind == Token::Kind::Close) {
                    if (--depth == 0) break;
                    continue;
                }
                if (depth != 1) continue;
                if (!EqualsNoCase(token.text, "betakey")) continue;
                if (j + 1 >= tokens.size()) return std::nullopt;
                if (tokens[j + 1].kind != Token::Kind::Value) return std::nullopt;
                if (tokens[j + 1].text.empty()) return std::nullopt;
                return std::string(tokens[j + 1].text);
            }
        }
        return std::nullopt;
    }

    // Manifest text with "betakey" set to `branch` inside `section`.
    //
    // Returns nullopt when there is nothing to do: `branch` is empty (that is
    // Steam's own "no test branch" state, which must never be re-asserted) or
    // the manifest already carries exactly that value. The existing key is
    // rewritten in place, so the rest of Steam's formatting — indentation,
    // ordering, CRLF — is preserved; a missing section is appended.
    inline std::optional<std::string> WithBetaKey(std::string_view text,
                                                  std::string_view section,
                                                  std::string_view branch) {
        if (branch.empty() || section.empty()) return std::nullopt;
        const auto tokens = Tokenize(text);
        const std::string nl = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        const std::string quotedBranch = "\"" + std::string(branch) + "\"";

        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].kind != Token::Kind::Value) continue;
            if (!EqualsNoCase(tokens[i].text, section)) continue;
            if (tokens[i + 1].kind != Token::Kind::Open) continue;

            const size_t openEnd = tokens[i + 1].end;
            int depth = 1;
            for (size_t j = i + 2; j < tokens.size(); ++j) {
                const Token& token = tokens[j];
                if (token.kind == Token::Kind::Open) { ++depth; continue; }
                if (token.kind == Token::Kind::Close) {
                    if (--depth == 0) break;
                    continue;
                }
                if (depth != 1) continue;
                if (!EqualsNoCase(token.text, "betakey")) continue;
                if (j + 1 >= tokens.size()) return std::nullopt;
                const Token& value = tokens[j + 1];
                if (value.kind != Token::Kind::Value) return std::nullopt;
                if (value.text == branch) return std::nullopt;          // already correct

                std::string patched(text);
                patched.replace(value.begin, value.end - value.begin, quotedBranch);
                return patched;
            }

            // Section present, no key: put one right after its opening brace.
            std::string patched(text);
            patched.insert(openEnd, nl + "\t\t\"betakey\"\t\t" + quotedBranch);
            return patched;
        }

        // Section missing: add it as the last child of the enclosing block.
        for (size_t i = tokens.size(); i-- > 0;) {
            if (tokens[i].kind != Token::Kind::Close) continue;
            std::string patched(text);
            const std::string block =
                "\t\"" + std::string(section) + "\"" + nl +
                "\t{" + nl +
                "\t\t\"betakey\"\t\t" + quotedBranch + nl +
                "\t}" + nl;
            patched.insert(tokens[i].begin, block);
            return patched;
        }
        return std::nullopt;
    }

    // Every library root listed in steamapps/libraryfolders.vdf ("path" values),
    // with VDF escaping undone. The Steam root is not included — callers add it.
    inline std::vector<std::string> LibraryFolders(std::string_view libraryFoldersVdf) {
        std::vector<std::string> folders;
        const auto tokens = Tokenize(libraryFoldersVdf);
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].kind != Token::Kind::Value) continue;
            if (!EqualsNoCase(tokens[i].text, "path")) continue;
            if (tokens[i + 1].kind != Token::Kind::Value) continue;
            if (tokens[i + 1].text.empty()) continue;
            folders.push_back(Unescape(tokens[i + 1].text));
        }
        return folders;
    }

    // ── installed build ────────────────────────────────────────────────────────
    // The depot set of what is actually on disk:
    //   "InstalledDepots" { "<depotId>" { "manifest" "<gid>" "size" "<bytes>" } }
    // ("MountedDepots" on older clients). This is the record of the build the
    // machine is on, which is what a test branch is mounted from.
    struct InstalledDepot {
        uint32_t depotId = 0;
        uint64_t gid     = 0;
        uint64_t size    = 0;
    };

    namespace detail {
        inline uint64_t ParseDigits(std::string_view text) {
            uint64_t value = 0;
            for (char ch : text) {
                if (ch < '0' || ch > '9') return 0;
                value = value * 10 + static_cast<uint64_t>(ch - '0');
            }
            return value;
        }
    }

    inline std::vector<InstalledDepot> FindInstalledDepots(std::string_view text) {
        std::vector<InstalledDepot> depots;
        const auto tokens = Tokenize(text);
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].kind != Token::Kind::Value) continue;
            if (!EqualsNoCase(tokens[i].text, "InstalledDepots") &&
                !EqualsNoCase(tokens[i].text, "MountedDepots")) continue;
            if (tokens[i + 1].kind != Token::Kind::Open) continue;

            int depth = 1;
            for (size_t j = i + 2; j + 1 < tokens.size(); ++j) {
                if (tokens[j].kind == Token::Kind::Open) { ++depth; continue; }
                if (tokens[j].kind == Token::Kind::Close) {
                    if (--depth == 0) break;
                    continue;
                }
                if (depth != 1 || tokens[j].kind != Token::Kind::Value) continue;
                if (tokens[j + 1].kind != Token::Kind::Open) continue;   // depot block

                InstalledDepot depot;
                depot.depotId = static_cast<uint32_t>(detail::ParseDigits(tokens[j].text));

                int inner = 1;
                for (size_t k = j + 2; k + 1 < tokens.size(); ++k) {
                    if (tokens[k].kind == Token::Kind::Open) { ++inner; continue; }
                    if (tokens[k].kind == Token::Kind::Close) {
                        if (--inner == 0) break;
                        continue;
                    }
                    if (inner != 1 || tokens[k].kind != Token::Kind::Value) continue;
                    if (tokens[k + 1].kind != Token::Kind::Value) continue;
                    if (EqualsNoCase(tokens[k].text, "manifest")) {
                        depot.gid = detail::ParseDigits(tokens[k + 1].text);
                    } else if (EqualsNoCase(tokens[k].text, "size")) {
                        depot.size = detail::ParseDigits(tokens[k + 1].text);
                    }
                }

                if (depot.depotId && depot.gid) depots.push_back(depot);
            }
            if (!depots.empty()) break;
        }
        return depots;
    }

} // namespace SteamConfig::AppManifestBeta
