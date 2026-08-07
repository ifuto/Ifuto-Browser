/* Ifuto — 拡張 manifest パーサ実装（文法は ext_manifest.h のコメントが唯一の正）。 */
#include "ext_manifest.h"
#include <string.h>
#include <stdio.h>

static bool m_charset(IfStr s) { /* [A-Za-z0-9_.-] のみ（表示・パス両安全面の構造排除） */
    if (s.n == 0) return false;
    for (u32 i = 0; i < s.n; i++) {
        char c = s.p[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static u32 m_copy_bounded(char *dst, u32 cap, IfStr s) {
    u32 n = s.n < cap - 1 ? s.n : cap - 1;
    memcpy(dst, s.p, n); dst[n] = 0;
    return n;
}

static IfStr m_trim(IfStr s) {
    while (s.n && (s.p[0] == ' ' || s.p[0] == '\t')) { s.p++; s.n--; }
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t' || s.p[s.n - 1] == '\r')) s.n--;
    return s;
}

static void m_err(char *err, u32 cap, u32 line, const char *what) {
    snprintf(err, cap, "manifest: line %u: %s", line, what);
}

bool if_ext_manifest_parse(IfStr src, IfExtManifest *out, char *err, u32 errcap) {
    if (errcap) err[0] = 0;
    memset(out, 0, sizeof *out);
    if (src.n > IF_EXT_MANIFEST_CAP) {
        snprintf(err, errcap, "manifest: too large (%u bytes)", src.n);
        return false;
    }
    bool seen_name = false, seen_ver = false, seen_entry = false, seen_perm = false;
    u32 lineno = 0;
    const char *p = src.p, *end = src.p + src.n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        IfStr line = if_str(p, nl ? (u32)(nl - p) : (u32)(end - p));
        p = nl ? nl + 1 : end;
        lineno++;
        IfStr t = m_trim(line);
        if (t.n == 0 || t.p[0] == '#') continue;
        const char *colon = memchr(t.p, ':', t.n);
        if (!colon) { m_err(err, errcap, lineno, "missing ':'"); return false; }
        IfStr key = m_trim(if_str(t.p, (u32)(colon - t.p)));
        IfStr val = m_trim(if_str(colon + 1, t.n - (u32)(colon - t.p) - 1));
        #define KEY_IS(lit) (key.n == sizeof(lit) - 1 && memcmp(key.p, lit, key.n) == 0)
        if (KEY_IS("name")) {
            if (seen_name) { m_err(err, errcap, lineno, "duplicate key \"name\""); return false; }
            seen_name = true;
            if (!m_charset(val) || val.n >= IF_EXT_NAME_CAP) { m_err(err, errcap, lineno, "bad name"); return false; }
            m_copy_bounded(out->name, sizeof out->name, val);
        } else if (KEY_IS("version")) {
            if (seen_ver) { m_err(err, errcap, lineno, "duplicate key \"version\""); return false; }
            seen_ver = true;
            if (!m_charset(val) || val.n >= IF_EXT_VER_CAP) { m_err(err, errcap, lineno, "bad version"); return false; }
            m_copy_bounded(out->version, sizeof out->version, val);
        } else if (KEY_IS("entry")) {
            if (seen_entry) { m_err(err, errcap, lineno, "duplicate key \"entry\""); return false; }
            seen_entry = true;
            if (!m_charset(val) || val.n >= IF_EXT_ENTRY_CAP ||
                val.p[0] == '.' /* ".."・隠しファイル・カレント示唆を全て拒否 */) {
                m_err(err, errcap, lineno, "bad entry (basename only)"); return false;
            }
            /* charset で '/' '\\' は既に排除済み（二重防御の明記） */
            m_copy_bounded(out->entry, sizeof out->entry, val);
        } else if (KEY_IS("permissions")) {
            if (seen_perm) { m_err(err, errcap, lineno, "duplicate key \"permissions\""); return false; }
            seen_perm = true;
            /* ',' 区切りトークン走査。E1: ≤1 つの有効ケイパビリティ */
            u8 perm = IF_EXT_PERM_NONE;
            u32 n_tok = 0;
            IfStr rest = val;
            while (rest.n) {
                const char *comma = memchr(rest.p, ',', rest.n);
                IfStr tok = m_trim(comma ? if_str(rest.p, (u32)(comma - rest.p)) : rest);
                rest = comma ? if_str(comma + 1, rest.n - (u32)(comma - rest.p) - 1) : if_str(rest.p + rest.n, 0);
                if (tok.n == 0) continue; /* "a,,b" の空片は寛容（無害） */
                n_tok++;
                if (tok.n == 6 && memcmp(tok.p, "status", 6) == 0) perm = IF_EXT_PERM_STATUS;
                else if (tok.n == 3 && memcmp(tok.p, "log", 3) == 0) perm = IF_EXT_PERM_LOG;
                else { snprintf(err, errcap, "manifest: line %u: unknown permission \"%.*s\"",
                                lineno, (int)(tok.n > 24 ? 24 : tok.n), tok.p); return false; }
            }
            if (n_tok > 1) { m_err(err, errcap, lineno, "E1: at most one permission"); return false; }
            out->perm = perm;
        } else {
            snprintf(err, errcap, "manifest: line %u: unknown key \"%.*s\"",
                     lineno, (int)(key.n > 24 ? 24 : key.n), key.p);
            return false;
        }
        #undef KEY_IS
    }
    if (!seen_name || !seen_ver || !seen_entry) {
        snprintf(err, errcap, "manifest: required key missing (%s%s%s)",
                 seen_name ? "" : "name ", seen_ver ? "" : "version ", seen_entry ? "" : "entry");
        return false;
    }
    return true;
}

const char *if_ext_perm_name(u8 perm) {
    return perm == IF_EXT_PERM_STATUS ? "status" : perm == IF_EXT_PERM_LOG ? "log" : "none";
}
