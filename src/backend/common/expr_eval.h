#pragma once
// Tiny arithmetic-expression evaluator shared by the netlist parser (backend)
// and the schematic property panel (frontend).
//
//   expr   := term  (('+'|'-') term)*
//   term   := factor (('*'|'/') factor)*
//   factor := ('+'|'-') factor | NUMBER | IDENT | '(' expr ')'
//
// NUMBER accepts strtod syntax followed by an optional SPICE magnitude suffix
// (case-insensitive): f=1e-15 p=1e-12 n=1e-9 u=1e-6 m=1e-3 k=1e3 Meg=1e6 g=1e9.
// IDENT resolves through a caller-supplied variable map (keys stored UPPERCASE).
//
// tryEval() is strict: the whole string must parse (unknown suffix, trailing
// garbage, undefined variable, or division by zero all fail). Callers keep
// their legacy lenient path as a fallback so existing netlists don't regress.

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace exprv {

using VarMap = std::unordered_map<std::string, double>;

namespace detail {

struct Parser {
    const char* p;
    const VarMap* vars;
    bool ok = true;

    void skipWs() { while (*p == ' ' || *p == '\t') ++p; }

    static bool suffixFactor(const std::string& sufUpper, double& factor) {
        if (sufUpper == "MEG") { factor = 1e6;   return true; }
        if (sufUpper.size() != 1) return false;
        switch (sufUpper[0]) {
            case 'F': factor = 1e-15; return true;
            case 'P': factor = 1e-12; return true;
            case 'N': factor = 1e-9;  return true;
            case 'U': factor = 1e-6;  return true;
            case 'M': factor = 1e-3;  return true;
            case 'K': factor = 1e3;   return true;
            case 'G': factor = 1e9;   return true;
            default:  return false;
        }
    }

    double parseFactor() {
        skipWs();
        if (*p == '+') { ++p; return parseFactor(); }
        if (*p == '-') { ++p; return -parseFactor(); }
        if (*p == '(') {
            ++p;
            double v = parseExpr();
            skipWs();
            if (*p != ')') { ok = false; return 0.0; }
            ++p;
            return v;
        }
        if (std::isdigit((unsigned char)*p) || *p == '.') {
            char* end = nullptr;
            double v = std::strtod(p, &end);
            if (end == p) { ok = false; return 0.0; }
            p = end;
            // Optional magnitude suffix: consume the full trailing identifier
            // so "1kx" is an error rather than silently reading as 1e3.
            std::string suf;
            while (std::isalpha((unsigned char)*p) || *p == '_')
                suf += (char)std::toupper((unsigned char)*p++);
            if (!suf.empty()) {
                double f;
                if (!suffixFactor(suf, f)) { ok = false; return 0.0; }
                v *= f;
            }
            return v;
        }
        if (std::isalpha((unsigned char)*p) || *p == '_') {
            std::string name;
            while (std::isalnum((unsigned char)*p) || *p == '_')
                name += (char)std::toupper((unsigned char)*p++);
            if (!vars) { ok = false; return 0.0; }
            auto it = vars->find(name);
            if (it == vars->end()) { ok = false; return 0.0; }
            return it->second;
        }
        ok = false;
        return 0.0;
    }

    double parseTerm() {
        double v = parseFactor();
        for (;;) {
            skipWs();
            if (*p == '*') { ++p; v *= parseFactor(); }
            else if (*p == '/') {
                ++p;
                double d = parseFactor();
                if (d == 0.0) { ok = false; return 0.0; }
                v /= d;
            }
            else return v;
        }
    }

    double parseExpr() {
        double v = parseTerm();
        for (;;) {
            skipWs();
            if (*p == '+') { ++p; v += parseTerm(); }
            else if (*p == '-') { ++p; v -= parseTerm(); }
            else return v;
        }
    }
};

} // namespace detail

// Strict full-string evaluation. Returns false (out untouched) on any error.
inline bool tryEval(const std::string& s, const VarMap* vars, double& out) {
    if (s.empty()) return false;
    detail::Parser ps{s.c_str(), vars};
    double v = ps.parseExpr();
    ps.skipWs();
    if (!ps.ok || *ps.p != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

inline double evalOr(const std::string& s, const VarMap* vars, double fallback) {
    double v;
    return tryEval(s, vars, v) ? v : fallback;
}

} // namespace exprv
