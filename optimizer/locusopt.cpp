#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using std::string;
using std::vector;

struct ArrayAccess {
    string name;
    vector<string> indices;
    bool is_write = false;
};

struct LoopVar {
    string name;
    string start;
    string end;
    int step = 1;
    string header;
    bool valid = false;
};

struct LoopNest {
    vector<LoopVar> loops;
    vector<ArrayAccess> accesses;
    string body;
    size_t start = 0;
    size_t end = 0;
};

struct AccessQuality {
    ArrayAccess access;
    string quality;
    string reason;
};

struct NestAnalysis {
    string overall_quality;
    bool recommend_interchange = false;
    bool recommend_tiling = false;
    vector<AccessQuality> access_qualities;
    vector<string> notes;
};

struct DependenceResult {
    bool safe_interchange = false;
    bool safe_tiling = false;
    string reason;
    vector<string> details;
};

static string read_file(const string &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Unable to read file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_file(const string &path, const string &content) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Unable to write file: " + path);
    }
    out << content;
}

static string ltrim(string s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        i++;
    }
    return s.substr(i);
}

static string rtrim(string s) {
    if (s.empty()) {
        return s;
    }
    size_t i = s.size() - 1;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        if (i == 0) {
            return "";
        }
        i--;
    }
    return s.substr(0, i + 1);
}

static string trim(string s) {
    return rtrim(ltrim(std::move(s)));
}

static bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static string strip_comments(const string &input) {
    string out;
    bool in_line = false;
    bool in_block = false;
    bool in_string = false;
    bool in_char = false;
    bool escape = false;

    for (size_t i = 0; i < input.size(); i++) {
        char c = input[i];
        char n = (i + 1 < input.size()) ? input[i + 1] : '\0';

        if (in_line) {
            if (c == '\n') {
                in_line = false;
                out += c;
            } else {
                out += ' ';
            }
            continue;
        }

        if (in_block) {
            if (c == '*' && n == '/') {
                in_block = false;
                out += ' ';
                out += ' ';
                i++;
                continue;
            }
            if (c == '\n') {
                out += '\n';
            } else {
                out += ' ';
            }
            continue;
        }

        if (in_string) {
            out += c;
            if (!escape && c == '"') {
                in_string = false;
            }
            escape = (!escape && c == '\\');
            continue;
        }

        if (in_char) {
            out += c;
            if (!escape && c == '\'') {
                in_char = false;
            }
            escape = (!escape && c == '\\');
            continue;
        }

        if (c == '"' && !in_char) {
            in_string = true;
            out += c;
            continue;
        }

        if (c == '\'' && !in_string) {
            in_char = true;
            out += c;
            continue;
        }

        if (c == '/' && n == '/') {
            in_line = true;
            out += ' ';
            out += ' ';
            i++;
            continue;
        }

        if (c == '/' && n == '*') {
            in_block = true;
            out += ' ';
            out += ' ';
            i++;
            continue;
        }

        out += c;
    }

    return out;
}

static size_t find_matching(const string &s, size_t pos, char open, char close) {
    int depth = 0;
    for (size_t i = pos; i < s.size(); i++) {
        if (s[i] == open) {
            depth++;
        } else if (s[i] == close) {
            depth--;
            if (depth == 0) {
                return i;
            }
        }
    }
    return string::npos;
}

static size_t find_next_for(const string &code, size_t pos, size_t end) {
    for (size_t i = pos; i + 2 < end; i++) {
        if (code[i] != 'f' || code[i + 1] != 'o' || code[i + 2] != 'r') {
            continue;
        }
        char before = (i == 0) ? '\0' : code[i - 1];
        char after = (i + 3 < code.size()) ? code[i + 3] : '\0';
        if (is_ident_char(before) || is_ident_char(after)) {
            continue;
        }
        return i;
    }
    return string::npos;
}

static vector<string> split(const string &s, char delim) {
    vector<string> parts;
    std::stringstream ss(s);
    string item;
    while (std::getline(ss, item, delim)) {
        parts.push_back(item);
    }
    return parts;
}

static LoopVar parse_loop_header(const string &header) {
    LoopVar lv;
    lv.header = trim(header);
    auto parts = split(header, ';');
    if (parts.size() != 3) {
        return lv;
    }

    string init = trim(parts[0]);
    string cond = trim(parts[1]);
    string step = trim(parts[2]);

    std::regex init_re(R"(([A-Za-z_]\w*)\s*=)");
    std::sregex_iterator it(init.begin(), init.end(), init_re);
    std::sregex_iterator end;
    string var_name;
    for (; it != end; ++it) {
        var_name = (*it)[1].str();
    }
    if (var_name.empty()) {
        return lv;
    }

    size_t eq = init.find_last_of('=');
    if (eq == string::npos) {
        return lv;
    }
    string start_expr = trim(init.substr(eq + 1));

    string op;
    size_t op_pos = cond.find("<=");
    if (op_pos != string::npos) {
        op = "<=";
    } else {
        op_pos = cond.find('<');
        if (op_pos == string::npos) {
            return lv;
        }
        op = "<";
    }
    string right = trim(cond.substr(op_pos + op.size()));
    if (right.empty()) {
        return lv;
    }

    string end_expr = right;
    if (op == "<=") {
        end_expr = "(" + right + ") + 1";
    }

    int step_val = 1;
    if (step.find("++") != string::npos) {
        step_val = 1;
    } else if (step.find("--") != string::npos) {
        step_val = -1;
    } else {
        size_t plus_eq = step.find("+=");
        if (plus_eq != string::npos) {
            string rhs = trim(step.substr(plus_eq + 2));
            try {
                step_val = std::stoi(rhs);
            } catch (...) {
                step_val = 1;
            }
        }
    }

    lv.name = var_name;
    lv.start = start_expr.empty() ? "0" : start_expr;
    lv.end = end_expr;
    lv.step = step_val;
    lv.valid = true;
    return lv;
}

struct LoopNode {
    LoopVar var;
    string body;
    size_t start = 0;
    size_t end = 0;
    bool valid = false;
};

static LoopNode parse_loop_at(const string &code, size_t pos) {
    LoopNode node;
    size_t open = code.find('(', pos);
    if (open == string::npos) {
        return node;
    }
    size_t close = find_matching(code, open, '(', ')');
    if (close == string::npos) {
        return node;
    }

    string header = code.substr(open + 1, close - open - 1);
    LoopVar lv = parse_loop_header(header);
    if (!lv.valid) {
        return node;
    }

    size_t body_start = close + 1;
    while (body_start < code.size() && std::isspace(static_cast<unsigned char>(code[body_start]))) {
        body_start++;
    }
    if (body_start >= code.size() || code[body_start] != '{') {
        return node;
    }
    size_t body_end = find_matching(code, body_start, '{', '}');
    if (body_end == string::npos) {
        return node;
    }

    node.var = lv;
    node.body = code.substr(body_start + 1, body_end - body_start - 1);
    node.start = pos;
    node.end = body_end + 1;
    node.valid = true;
    return node;
}

static bool is_loop_token(const string &s, size_t pos) {
    if (pos + 2 >= s.size()) {
        return false;
    }
    if (s[pos] != 'f' || s[pos + 1] != 'o' || s[pos + 2] != 'r') {
        return false;
    }
    char before = (pos == 0) ? '\0' : s[pos - 1];
    char after = (pos + 3 < s.size()) ? s[pos + 3] : '\0';
    return !is_ident_char(before) && !is_ident_char(after);
}

static bool body_is_single_loop(const string &body, LoopNode &inner) {
    size_t first = body.find_first_not_of(" \t\r\n");
    if (first == string::npos) {
        return false;
    }
    if (!is_loop_token(body, first)) {
        return false;
    }
    LoopNode parsed = parse_loop_at(body, first);
    if (!parsed.valid) {
        return false;
    }
    string tail = body.substr(parsed.end);
    if (!trim(tail).empty()) {
        return false;
    }
    inner = parsed;
    return true;
}

static vector<ArrayAccess> extract_accesses(const string &expr, bool write_first) {
    vector<ArrayAccess> accesses;
    std::regex access_re(R"(([A-Za-z_]\w*)\s*((\[[^\]]+\])+))");
    auto begin = std::sregex_iterator(expr.begin(), expr.end(), access_re);
    auto end = std::sregex_iterator();
    bool first = true;
    for (auto it = begin; it != end; ++it) {
        ArrayAccess acc;
        acc.name = (*it)[1].str();
        string indices_blob = (*it)[2].str();
        size_t idx = 0;
        while ((idx = indices_blob.find('[', idx)) != string::npos) {
            size_t close = indices_blob.find(']', idx + 1);
            if (close == string::npos) {
                break;
            }
            string idx_expr = trim(indices_blob.substr(idx + 1, close - idx - 1));
            acc.indices.push_back(idx_expr);
            idx = close + 1;
        }
        acc.is_write = write_first && first;
        accesses.push_back(acc);
        first = false;
    }
    return accesses;
}

static vector<ArrayAccess> collect_accesses(const string &body) {
    vector<ArrayAccess> accesses;
    auto stmts = split(body, ';');
    for (const auto &raw_stmt : stmts) {
        string stmt = trim(raw_stmt);
        if (stmt.empty()) {
            continue;
        }
        size_t eq = string::npos;
        for (size_t i = 0; i < stmt.size(); i++) {
            if (stmt[i] == '=') {
                char prev = (i > 0) ? stmt[i - 1] : '\0';
                char next = (i + 1 < stmt.size()) ? stmt[i + 1] : '\0';
                if (prev != '=' && next != '=' && prev != '!' && prev != '<' && prev != '>') {
                    eq = i;
                    break;
                }
            }
        }
        if (eq != string::npos) {
            string lhs = stmt.substr(0, eq);
            string rhs = stmt.substr(eq + 1);
            auto lhs_accesses = extract_accesses(lhs, true);
            auto rhs_accesses = extract_accesses(rhs, false);
            accesses.insert(accesses.end(), lhs_accesses.begin(), lhs_accesses.end());
            accesses.insert(accesses.end(), rhs_accesses.begin(), rhs_accesses.end());
        } else {
            auto reads = extract_accesses(stmt, false);
            accesses.insert(accesses.end(), reads.begin(), reads.end());
        }
    }
    return accesses;
}

static bool contains_var(const string &expr, const string &var) {
    std::regex pat("\\b" + var + "\\b");
    return std::regex_search(expr, pat);
}

static NestAnalysis analyse_nest(const LoopNest &nest) {
    NestAnalysis result;
    if (nest.loops.empty()) {
        result.overall_quality = "unknown";
        result.notes.push_back("Empty loop nest.");
        return result;
    }

    string inner_var = nest.loops.back().name;
    for (const auto &acc : nest.accesses) {
        AccessQuality aq;
        aq.access = acc;
        if (acc.indices.empty()) {
            aq.quality = "unknown";
            aq.reason = "no index information";
        } else {
            string last = acc.indices.back();
            size_t dims = acc.indices.size();
            if (contains_var(last, inner_var)) {
                aq.quality = "good";
                aq.reason = "inner var '" + inner_var + "' varies last index";
            } else if (dims == 1) {
                aq.quality = "good";
                aq.reason = "1-D access or scalar reuse";
            } else {
                bool inner_on_earlier = false;
                for (size_t i = 0; i + 1 < acc.indices.size(); i++) {
                    if (contains_var(acc.indices[i], inner_var)) {
                        inner_on_earlier = true;
                        break;
                    }
                }
                if (inner_on_earlier) {
                    aq.quality = "poor";
                    aq.reason = "inner var varies non-last index";
                } else {
                    aq.quality = "good";
                    aq.reason = "inner var does not affect index";
                }
            }
        }
        result.access_qualities.push_back(aq);
    }

    bool has_good = false;
    bool has_poor = false;
    bool has_unknown = false;
    for (const auto &aq : result.access_qualities) {
        if (aq.quality == "good") {
            has_good = true;
        } else if (aq.quality == "poor") {
            has_poor = true;
        } else {
            has_unknown = true;
        }
    }

    if (!has_good && !has_poor) {
        result.overall_quality = "unknown";
    } else if (has_good && !has_poor) {
        result.overall_quality = "good";
    } else if (!has_good && has_poor) {
        result.overall_quality = "poor";
    } else {
        result.overall_quality = "mixed";
    }

    int depth = static_cast<int>(nest.loops.size());
    if (result.overall_quality == "good") {
        result.notes.push_back("Access pattern is cache-friendly.");
        if (depth >= 2) {
            result.recommend_tiling = true;
            result.notes.push_back("Loop tiling can improve reuse.");
        }
    } else if (result.overall_quality == "poor" || result.overall_quality == "mixed") {
        if (depth >= 2) {
            string outer_var = nest.loops.front().name;
            int interchange_good = 0;
            int current_good = 0;
            for (const auto &aq : result.access_qualities) {
                if (aq.quality == "good") {
                    current_good++;
                }
                if (!aq.access.indices.empty()) {
                    string last = aq.access.indices.back();
                    if (contains_var(last, outer_var)) {
                        interchange_good++;
                    }
                }
            }
            if (interchange_good > current_good) {
                result.recommend_interchange = true;
                result.notes.push_back("Loop interchange improves locality.");
            } else {
                result.notes.push_back("Interchange does not improve locality.");
            }
            result.recommend_tiling = true;
            result.notes.push_back("Loop tiling recommended for cache reuse.");
        }
    }

    if (depth >= 3) {
        result.recommend_tiling = true;
        result.notes.push_back("3-level nest: consider tiling multiple loops.");
    }

    return result;
}

static bool is_affine(const string &expr) {
    std::regex non_affine(R"([/%\[\]]|[A-Za-z_]\w*\s*\()");
    return !std::regex_search(expr, non_affine);
}

static bool is_stencil_index(const string &expr, const vector<LoopVar> &loops) {
    std::regex stencil(R"(\b([A-Za-z_]\w*)\s*[+\-]\s*\d+)");
    std::smatch match;
    if (std::regex_search(expr, match, stencil)) {
        string var = match[1].str();
        for (const auto &lv : loops) {
            if (lv.name == var) {
                return true;
            }
        }
    }
    return false;
}

static DependenceResult check_dependence(const LoopNest &nest) {
    DependenceResult result;
    if (nest.loops.size() < 2) {
        result.reason = "Need at least 2 loop levels for interchange/tiling.";
        return result;
    }

    for (const auto &acc : nest.accesses) {
        for (const auto &idx : acc.indices) {
            if (!is_affine(idx)) {
                result.reason = "Non-affine index expression detected.";
                result.details.push_back("Non-affine index '" + idx + "' in " +
                                         acc.name + ".");
                return result;
            }
        }
    }

    bool loop_carried = false;
    for (const auto &acc : nest.accesses) {
        if (acc.is_write) {
            for (const auto &idx : acc.indices) {
                if (is_stencil_index(idx, nest.loops)) {
                    loop_carried = true;
                    result.details.push_back("Write with stencil index '" + idx +
                                             "' on '" + acc.name + "'.");
                }
            }
        }
    }

    if (loop_carried) {
        result.reason = "Loop-carried dependence detected.";
        return result;
    }

    result.safe_interchange = nest.loops.size() == 2;
    result.safe_tiling = true;
    result.reason = "No loop-carried dependencies detected.";
    return result;
}

static string indent_block(const string &body, const string &indent) {
    std::stringstream ss(body);
    string line;
    string out;
    bool first = true;
    while (std::getline(ss, line)) {
        if (!first) {
            out += "\n";
        }
        if (!trim(line).empty()) {
            out += indent + trim(line);
        } else {
            out += indent;
        }
        first = false;
    }
    return out;
}

static string get_indent(const string &code, size_t pos) {
    size_t line_start = code.rfind('\n', pos);
    if (line_start == string::npos) {
        line_start = 0;
    } else {
        line_start += 1;
    }
    string indent;
    for (size_t i = line_start; i < code.size(); i++) {
        char c = code[i];
        if (c == ' ' || c == '\t') {
            indent += c;
        } else {
            break;
        }
    }
    return indent;
}

static string build_simple_nest(const vector<LoopVar> &loops, const string &body,
                                const string &base_indent) {
    const string indent_unit = "    ";
    string out;
    for (size_t i = 0; i < loops.size(); i++) {
        out += base_indent + string(i * indent_unit.size(), ' ') + "for (" +
               loops[i].header + ") {\n";
    }
    out += indent_block(trim(body), base_indent + string(loops.size() * indent_unit.size(), ' '));
    out += "\n";
    for (size_t i = loops.size(); i-- > 0;) {
        out += base_indent + string(i * indent_unit.size(), ' ') + "}\n";
    }
    return out;
}

static string build_tiled_nest(const LoopVar &outer, const LoopVar &inner,
                               const string &body, const string &base_indent,
                               int tile_size) {
    const string indent_unit = "    ";
    string outer_tile = outer.name + "_tile";
    string inner_tile = inner.name + "_tile";

    string out;
    out += base_indent + "for (int " + outer_tile + " = " + outer.start + "; " +
           outer_tile + " < " + outer.end + "; " + outer_tile + " += " +
           std::to_string(tile_size) + ") {\n";
    out += base_indent + indent_unit + "for (int " + inner_tile + " = " + inner.start +
           "; " + inner_tile + " < " + inner.end + "; " + inner_tile + " += " +
           std::to_string(tile_size) + ") {\n";
    out += base_indent + indent_unit + indent_unit + "for (" + outer.name + " = " +
           outer_tile + "; " + outer.name + " < LOCUSOPT_MIN(" + outer_tile + " + " +
           std::to_string(tile_size) + ", " + outer.end + "); " + outer.name +
           " += " + std::to_string(outer.step) + ") {\n";
    out += base_indent + indent_unit + indent_unit + indent_unit + "for (" +
           inner.name + " = " + inner_tile + "; " + inner.name + " < LOCUSOPT_MIN(" +
           inner_tile + " + " + std::to_string(tile_size) + ", " + inner.end +
           "); " + inner.name + " += " + std::to_string(inner.step) + ") {\n";
    out += indent_block(trim(body),
                        base_indent + indent_unit + indent_unit + indent_unit + indent_unit);
    out += "\n";
    out += base_indent + indent_unit + indent_unit + indent_unit + "}\n";
    out += base_indent + indent_unit + indent_unit + "}\n";
    out += base_indent + indent_unit + "}\n";
    out += base_indent + "}\n";
    return out;
}

static LoopNest parse_perfect_nest(const string &code, size_t pos) {
    LoopNest nest;
    LoopNode current = parse_loop_at(code, pos);
    if (!current.valid) {
        return nest;
    }

    nest.start = current.start;
    nest.end = current.end;
    nest.loops.push_back(current.var);

    LoopNode inner;
    while (body_is_single_loop(current.body, inner)) {
        nest.loops.push_back(inner.var);
        current = inner;
    }
    nest.body = current.body;
    nest.accesses = collect_accesses(current.body);
    return nest;
}

struct Replacement {
    size_t start;
    size_t end;
    string text;
};

static string ensure_min_macro(const string &code) {
    if (code.find("#define LOCUSOPT_MIN") != string::npos) {
        return code;
    }
    string macro =
        "#ifndef LOCUSOPT_MIN\n"
        "#define LOCUSOPT_MIN(a, b) ((a) < (b) ? (a) : (b))\n"
        "#endif\n\n";
    return macro + code;
}

static string optimize_code(const string &code, int tile_size, bool do_interchange,
                            bool do_tiling, const string &func_name) {
    string clean = strip_comments(code);
    size_t start_range = 0;
    size_t end_range = clean.size();
    if (!func_name.empty()) {
        std::regex func_re(func_name + R"(\s*\([^;{]*\)\s*\{)");
        std::smatch match;
        if (std::regex_search(clean, match, func_re)) {
            size_t brace = clean.find('{', match.position(0));
            size_t brace_end = find_matching(clean, brace, '{', '}');
            if (brace_end != string::npos) {
                start_range = brace + 1;
                end_range = brace_end;
            }
        }
    }

    vector<Replacement> replacements;
    bool needs_min = false;
    size_t pos = start_range;
    while (true) {
        size_t for_pos = find_next_for(clean, pos, end_range);
        if (for_pos == string::npos) {
            break;
        }
        LoopNest nest = parse_perfect_nest(clean, for_pos);
        if (nest.loops.size() < 2) {
            pos = for_pos + 3;
            continue;
        }

        NestAnalysis analysis = analyse_nest(nest);
        DependenceResult dep = check_dependence(nest);

        bool apply_interchange = do_interchange && analysis.recommend_interchange && dep.safe_interchange;
        bool apply_tiling = do_tiling && analysis.recommend_tiling && dep.safe_tiling;

        if (!apply_interchange && !apply_tiling) {
            pos = nest.end;
            continue;
        }

        if (nest.loops.size() != 2) {
            pos = nest.end;
            continue;
        }

        vector<LoopVar> loops = nest.loops;
        if (apply_interchange) {
            std::swap(loops[0], loops[1]);
        }

        string indent = get_indent(clean, nest.start);
        string replacement;
        if (apply_tiling) {
            if (loops[0].step != 1 || loops[1].step != 1) {
                replacement = build_simple_nest(loops, nest.body, indent);
            } else {
                replacement = build_tiled_nest(loops[0], loops[1], nest.body, indent, tile_size);
                needs_min = true;
            }
        } else {
            replacement = build_simple_nest(loops, nest.body, indent);
        }

        replacements.push_back({nest.start, nest.end, replacement});
        pos = nest.end;
    }

    if (replacements.empty()) {
        return code;
    }

    string optimized = code;
    std::sort(replacements.begin(), replacements.end(),
              [](const Replacement &a, const Replacement &b) {
                  return a.start > b.start;
              });
    for (const auto &rep : replacements) {
        optimized.replace(rep.start, rep.end - rep.start, rep.text);
    }

    if (needs_min) {
        optimized = ensure_min_macro(optimized);
    }
    return optimized;
}

static void print_analysis(const string &code, const string &filename,
                           const string &func_name) {
    string clean = strip_comments(code);
    size_t start_range = 0;
    size_t end_range = clean.size();
    if (!func_name.empty()) {
        std::regex func_re(func_name + R"(\s*\([^;{]*\)\s*\{)");
        std::smatch match;
        if (std::regex_search(clean, match, func_re)) {
            size_t brace = clean.find('{', match.position(0));
            size_t brace_end = find_matching(clean, brace, '{', '}');
            if (brace_end != string::npos) {
                start_range = brace + 1;
                end_range = brace_end;
            }
        }
    }

    std::cout << "\nLocusOpt CPP — Analysis Report\n";
    std::cout << "========================================\n";
    std::cout << "File: " << filename << "\n\n";

    size_t pos = start_range;
    int nest_num = 1;
    while (true) {
        size_t for_pos = find_next_for(clean, pos, end_range);
        if (for_pos == string::npos) {
            break;
        }
        LoopNest nest = parse_perfect_nest(clean, for_pos);
        if (nest.loops.empty()) {
            pos = for_pos + 3;
            continue;
        }

        std::cout << "Loop Nest #" << nest_num++ << ":\n";
        std::cout << " Structure: ";
        for (size_t i = 0; i < nest.loops.size(); i++) {
            const auto &lv = nest.loops[i];
            std::cout << "for " << lv.name << " : [" << lv.start << ", " << lv.end
                      << ")";
            if (i + 1 < nest.loops.size()) {
                std::cout << " -> ";
            }
        }
        std::cout << "\n";

        NestAnalysis analysis = analyse_nest(nest);
        std::cout << " Locality: " << analysis.overall_quality << "\n";
        std::cout << " Accesses:\n";
        for (const auto &aq : analysis.access_qualities) {
            std::cout << "    " << aq.access.name;
            for (const auto &idx : aq.access.indices) {
                std::cout << "[" << idx << "]";
            }
            std::cout << (aq.access.is_write ? " (write) " : " (read) ");
            std::cout << aq.quality << ": " << aq.reason << "\n";
        }
        for (const auto &note : analysis.notes) {
            std::cout << "  -> " << note << "\n";
        }

        DependenceResult dep = check_dependence(nest);
        std::cout << "  Dependence check:\n";
        std::cout << "    Safe interchange: " << (dep.safe_interchange ? "yes" : "no") << "\n";
        std::cout << "    Safe tiling: " << (dep.safe_tiling ? "yes" : "no") << "\n";
        std::cout << "    " << dep.reason << "\n";
        for (const auto &detail : dep.details) {
            std::cout << "      - " << detail << "\n";
        }

        pos = nest.end;
        std::cout << "\n";
    }
}

static void print_usage() {
    std::cout << "Usage:\n";
    std::cout << "  locusopt-cpp analyze <file> [--func NAME]\n";
    std::cout << "  locusopt-cpp optimize <file> [--output FILE] [--func NAME] [--tile N]"
                 " [--no-interchange] [--no-tiling]\n";
}

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }

    string command = argv[1];
    string source = argv[2];
    string func_name;
    string output_path;
    int tile_size = 32;
    bool allow_interchange = true;
    bool allow_tiling = true;

    for (int i = 3; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--func" && i + 1 < argc) {
            func_name = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--tile" && i + 1 < argc) {
            tile_size = std::stoi(argv[++i]);
        } else if (arg == "--no-interchange") {
            allow_interchange = false;
        } else if (arg == "--no-tiling") {
            allow_tiling = false;
        }
    }

    try {
        string code = read_file(source);
        if (command == "analyze") {
            print_analysis(code, source, func_name);
            return 0;
        }
        if (command == "optimize") {
            string optimized = optimize_code(code, tile_size, allow_interchange,
                                             allow_tiling, func_name);
            if (output_path.empty()) {
                std::cout << optimized;
            } else {
                write_file(output_path, optimized);
            }
            return 0;
        }
    } catch (const std::exception &ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }

    print_usage();
    return 1;
}
