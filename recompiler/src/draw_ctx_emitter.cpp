#include "draw_ctx_emitter.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <regex>
#include <sstream>

#include "fmt/format.h"

namespace PSXRecomp {

namespace {

bool is_hex8(const std::string& s, size_t pos) {
    if (pos + 8 > s.size()) return false;
    for (size_t i = pos; i < pos + 8; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Every func_XXXXXXXX / psx_alias_body_XXXXXXXX identifier in `code`.
void symbol_refs(const std::string& code, std::set<std::string>& out) {
    static const char* prefixes[] = {"func_", "psx_alias_body_"};
    for (const char* prefix : prefixes) {
        const std::string p = prefix;
        for (size_t at = code.find(p); at != std::string::npos; at = code.find(p, at + 1)) {
            if (at > 0 && ident_char(code[at - 1])) continue;
            const size_t hex = at + p.size();
            if (!is_hex8(code, hex)) continue;
            if (hex + 8 < code.size() && ident_char(code[hex + 8])) continue;
            out.insert(code.substr(at, p.size() + 8));
        }
    }
}

// Constant CPS transfers: `cpu->pc = 0xXXXXXXXXu;`.
void cps_targets(const std::string& code, std::set<uint32_t>& out) {
    const std::string p = "cpu->pc = 0x";
    for (size_t at = code.find(p); at != std::string::npos; at = code.find(p, at + 1)) {
        const size_t hex = at + p.size();
        if (!is_hex8(code, hex) || hex + 9 > code.size() || code[hex + 8] != 'u') continue;
        out.insert(static_cast<uint32_t>(std::strtoul(code.substr(hex, 8).c_str(), nullptr, 16)));
    }
}

// `code` without its /* */ and // comments (scanning copy: emitted comments
// name functions and PCs that are not references).
std::string strip_comments(const std::string& code) {
    std::string out;
    out.reserve(code.size());
    for (size_t i = 0; i < code.size();) {
        if (code.compare(i, 2, "/*") == 0) {
            const size_t end = code.find("*/", i + 2);
            i = end == std::string::npos ? code.size() : end + 2;
            out += ' ';
        } else if (code.compare(i, 2, "//") == 0) {
            const size_t end = code.find('\n', i);
            i = end == std::string::npos ? code.size() : end;
        } else {
            out += code[i++];
        }
    }
    return out;
}

uint32_t name_addr(const std::string& name) {
    return static_cast<uint32_t>(std::strtoul(name.c_str() + name.size() - 8, nullptr, 16));
}

}  // namespace

std::string draw_ctx_clone_text(const std::string& code,
                                const std::set<std::string>& names) {
    std::string out;
    out.reserve(code.size() + code.size() / 16);
    size_t i = 0;
    static const std::string kStorePc = "g_debug_last_store_pc = 0x";
    static const std::pair<const char*, const char*> kStores[] = {
        {"cpu->write_word(", "psx_dctx_sw(cpu, "},
        {"cpu->write_half(", "psx_dctx_sh(cpu, "},
        {"cpu->write_byte(", "psx_dctx_sb(cpu, "},
    };
    while (i < code.size()) {
        const char c = code[i];
        // Store-PC breadcrumb: the main context's debug attribution.
        if (c == 'g' && code.compare(i, kStorePc.size(), kStorePc) == 0 &&
            is_hex8(code, i + kStorePc.size()) &&
            code.compare(i + kStorePc.size() + 8, 3, "u; ") == 0) {
            i += kStorePc.size() + 8 + 3;
            continue;
        }
        if (c == 'c') {
            bool replaced = false;
            for (const auto& [from, to] : kStores) {
                const size_t n = std::char_traits<char>::length(from);
                if (code.compare(i, n, from) == 0 && (i == 0 || !ident_char(code[i - 1]))) {
                    out += to;
                    i += n;
                    replaced = true;
                    break;
                }
            }
            if (replaced) continue;
        }
        if ((c == 'f' || c == 'p') && (i == 0 || !ident_char(code[i - 1]))) {
            const char* prefix = c == 'f' ? "func_" : "psx_alias_body_";
            const size_t n = std::char_traits<char>::length(prefix);
            if (code.compare(i, n, prefix) == 0 && is_hex8(code, i + n) &&
                (i + n + 8 >= code.size() || !ident_char(code[i + n + 8]))) {
                const std::string name = code.substr(i, n + 8);
                if (names.count(name)) {
                    out += c == 'f' ? "dctx_" : "dctx_";
                    out += c == 'f' ? name : name.substr(4);   // psx_alias_body_ -> dctx_alias_body_
                    i += n + 8;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

bool emit_draw_ctx(const GameDispatchUnit& unit,
                   const std::vector<GeneratedFunction>& funcs,
                   const std::string& ranges_manifest,
                   const std::string& decls_basename,
                   const std::string& stem,
                   const DrawCtxConfig& config,
                   std::string& out, DrawCtxReport& report, std::string& error) {
    const CodeGenerator& codegen = *unit.codegen;
    const auto& conts = codegen.cps_continuations();   // cont -> owner

    // Which generated function defines each symbol.
    std::map<std::string, size_t> defines;
    for (size_t i = 0; i < funcs.size(); ++i) {
        defines[funcs[i].function_name] = i;
        const std::string& code = funcs[i].full_code;
        const std::string p = "void psx_alias_body_";
        for (size_t at = code.find(p); at != std::string::npos; at = code.find(p, at + 1)) {
            if (is_hex8(code, at + p.size()))
                defines[code.substr(at + 5, 15 + 8)] = i;
        }
    }

    auto owner_of = [&](uint32_t pc, uint32_t* owner) {
        if (unit.dispatch_addrs.count(pc)) { *owner = pc; return true; }
        auto it = conts.find(pc);
        if (it != conts.end() && unit.dispatch_addrs.count(it->second)) {
            *owner = it->second;
            return true;
        }
        // Dispatch keys are runtime PCs; compile addresses equal them for
        // KSEG0 games (docs/SEGMENT_AWARE_CODE.md §5.2).
        return false;
    };

    const std::set<uint32_t> excluded(config.exclude.begin(), config.exclude.end());
    std::set<size_t> closure;
    std::set<std::string> names;
    std::deque<size_t> work;
    auto add = [&](const std::string& name) {
        auto it = defines.find(name);
        if (it == defines.end()) return false;
        if (closure.insert(it->second).second) work.push_back(it->second);
        names.insert(name);
        return true;
    };

    for (uint32_t root : config.roots) {
        uint32_t owner = 0;
        if (!owner_of(root, &owner) || !add(fmt::format("func_{:08X}", owner))) {
            error = fmt::format("threaded_draw root 0x{:08X} is not a compiled game "
                                "function entry or continuation", root);
            return false;
        }
    }
    std::set<uint32_t> unresolved, excluded_hits;
    while (!work.empty()) {
        const size_t idx = work.front();
        work.pop_front();
        const std::string code = strip_comments(funcs[idx].full_code);
        std::set<std::string> refs;
        symbol_refs(code, refs);
        for (const auto& r : refs) {
            if (!add(r)) {
                error = fmt::format("draw clone {} references {}, which no generated "
                                    "function defines", funcs[idx].function_name, r);
                return false;
            }
        }
        std::set<uint32_t> targets;
        cps_targets(code, targets);
        for (uint32_t t : targets) {
            uint32_t owner = 0;
            if (!owner_of(t, &owner)) { unresolved.insert(t); continue; }
            if (excluded.count(owner)) { excluded_hits.insert(owner); continue; }
            add(fmt::format("func_{:08X}", owner));
        }
        if (closure.size() > config.max_functions) {
            error = fmt::format("threaded_draw closure exceeds max_functions ({}); "
                                "add exclude entries or raise the limit",
                                config.max_functions);
            return false;
        }
    }
    report.functions = closure.size();
    report.unresolved.assign(unresolved.begin(), unresolved.end());
    report.excluded_hits.assign(excluded_hits.begin(), excluded_hits.end());

    // Owners in the clone set (func_ names of dispatchable entries).
    std::set<uint32_t> owners;
    for (const auto& n : names) {
        if (n.rfind("func_", 0) != 0) continue;
        const uint32_t a = name_addr(n);
        if (unit.dispatch_addrs.count(a)) owners.insert(a);
    }

    // Code ranges per owner, from the manifest (as emit_game_dispatch).
    using CodeRange = std::pair<uint32_t, uint32_t>;
    std::map<uint32_t, std::vector<CodeRange>> function_ranges;
    {
        std::istringstream input(ranges_manifest);
        std::string line;
        uint32_t current = 0;
        while (std::getline(input, line)) {
            std::istringstream fields(line);
            char kind = 0;
            fields >> kind;
            if (kind == 'F') {
                fields >> std::hex >> current;
                function_ranges[current];
            } else if (kind == 'R' && current != 0) {
                uint32_t lo = 0, len = 0;
                fields >> std::hex >> lo >> len;
                if (len) function_ranges[current].push_back({lo, len});
            }
        }
    }
    auto ranges_of = [&](uint32_t owner) -> const std::vector<CodeRange>* {
        auto exact = function_ranges.find(owner);
        if (exact != function_ranges.end() && !exact->second.empty()) return &exact->second;
        for (const auto& [entry, ranges] : function_ranges) {
            (void)entry;
            for (const auto& [lo, len] : ranges)
                if (owner >= lo && (uint64_t)owner < (uint64_t)lo + len) return &ranges;
        }
        return nullptr;
    };

    struct Row { uint32_t key, resume, owner, range_index, range_count; };
    std::vector<Row> rows;
    std::vector<CodeRange> flat;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> spans;
    auto span_of = [&](uint32_t owner) {
        auto it = spans.find(owner);
        if (it != spans.end()) return it->second;
        const uint32_t first = (uint32_t)flat.size();
        if (const auto* r = ranges_of(owner)) flat.insert(flat.end(), r->begin(), r->end());
        auto s = std::make_pair(first, (uint32_t)flat.size() - first);
        spans.emplace(owner, s);
        return s;
    };
    for (uint32_t owner : owners) {
        auto s = span_of(owner);
        rows.push_back({codegen.runtime_pc(owner), 0u, owner, s.first, s.second});
    }
    for (const auto& [cont, owner] : conts) {
        if (!owners.count(owner) || unit.dispatch_addrs.count(cont)) continue;
        auto s = span_of(owner);
        rows.push_back({codegen.runtime_pc(cont), codegen.runtime_pc(cont), owner,
                        s.first, s.second});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.key < b.key; });
    for (size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].key == rows[i - 1].key) {
            error = fmt::format("draw table rows share PC 0x{:08X}", rows[i].key);
            return false;
        }
    }
    report.rows = rows.size();

    std::ostringstream o;
    o << "/* Generated by PSXRecomp - lean draw-context clones. DO NOT EDIT.\n"
      << " * docs/THREADED_CONTEXTS.md: the [threaded_draw] roots and their static\n"
      << " * callees, compiled without the cycle/interrupt/debug model and run by\n"
      << " * psx_ctx_call() against a draw context's private state. */\n";
    o << "#undef PSX_ENABLE_BLOCK_CYCLES\n#undef PSX_COSIM\n";
    o << "#define PSX_DRAW_CTX_BUILD 1\n";
    o << "#include \"" << decls_basename << "\"\n";
    o << "#include \"psx_dctx_codegen.h\"\n\n";
    o << fmt::format("/* {} clone bodies, {} trampoline rows; roots:", closure.size(), rows.size());
    for (uint32_t r : config.roots) o << fmt::format(" 0x{:08X}", r);
    o << " */\n\n";
    for (const auto& n : names) {
        if (n.rfind("func_", 0) == 0)
            o << "void dctx_" << n << "(CPUState* cpu);\n";
        else
            o << "void dctx_" << n.substr(4) << "(CPUState* cpu, uint32_t entry);\n";
    }
    o << "\n";
    for (size_t idx : closure) {
        o << draw_ctx_clone_text(funcs[idx].full_code, names) << "\n";
    }
    o << "static const uint32_t k_dctx_ranges[] = {\n";
    if (flat.empty()) o << "    0u, 0u,\n";
    for (const auto& [lo, len] : flat) o << fmt::format("    0x{:08X}u, 0x{:X}u,\n", lo, len);
    o << "};\n\n";
    o << "static const PsxDctxRow k_dctx_rows[] = {\n";
    if (rows.empty()) o << "    {0u, 0u, 0u, 0u, 0},\n";
    for (const auto& r : rows)
        o << fmt::format("    {{0x{:08X}u, 0x{:08X}u, {}u, {}u, dctx_func_{:08X}}},\n",
                         r.key, r.resume, r.range_index, r.range_count, r.owner);
    o << "};\n\n";
    o << "static const uint32_t k_dctx_roots[] = {";
    if (config.roots.empty()) o << " 0u";
    for (size_t i = 0; i < config.roots.size(); ++i)
        o << (i ? ", " : " ") << fmt::format("0x{:08X}u", config.roots[i]);
    o << " };\n\n";
    o << "static const PsxDctxTable k_dctx_table = {\n";
    o << "    \"" << stem << "\",\n";
    o << fmt::format("    k_dctx_rows, {}u,\n", rows.size());
    o << fmt::format("    k_dctx_ranges, {}u,\n", flat.size());
    o << fmt::format("    k_dctx_roots, {}u,\n", config.roots.size());
    o << "};\n\n";
    o << "static void psx_dctx_register_game(void) {\n";
    o << (rows.empty() ? "    (void)k_dctx_table;\n" : "    psx_dctx_register_table(&k_dctx_table);\n");
    o << "}\n";
    o << "#if defined(_MSC_VER)\n";
    o << "#pragma section(\".CRT$XCU\", read)\n";
    o << "__declspec(allocate(\".CRT$XCU\")) static void (*psx_dctx_register_ctor)(void) = psx_dctx_register_game;\n";
    o << "#else\n";
    o << "__attribute__((constructor)) static void psx_dctx_register_ctor(void) { psx_dctx_register_game(); }\n";
    o << "#endif\n";
    out = o.str();
    report.lines = (size_t)std::count(out.begin(), out.end(), '\n');
    return true;
}

}  // namespace PSXRecomp
