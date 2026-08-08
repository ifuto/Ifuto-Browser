/* Ifuto Browser — 文字コード層（設計凍結は charset.h / docs/CHARSET.md） */
#include "charset.h"
#include <string.h>
#include "charset_tables_gen.h"

/* ---- 小道具 ---- */

static u8 ci_lo(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

static bool ci_eqn(const char *p, u32 n, const char *word) {
    u32 wl = (u32)strlen(word);
    if (n != wl) return false;
    for (u32 i = 0; i < n; i++)
        if (ci_lo((u8)p[i]) != (u8)word[i]) return false;
    return true;
}

/* ---- ラベル正規化（WHATWG encoding labels の日本語関連 + 実害のある utf-8 別名） ---- */

IfEnc if_charset_label(const char *p, u32 n) {
    while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    /* WHATWG shift_jis ラベル群 + de-facto（cp932/windows-31j を同一系として受理） */
    static const char *const SJ[] = { "shift_jis", "shift-jis", "sjis", "csshiftjis",
        "ms_kanji", "windows-31j", "x-sjis", "cp932", "ms932", "x-ms-cp932" };
    static const char *const EJ[] = { "euc-jp", "cseucpkdfmtjapanese", "x-euc-jp" };
    static const char *const U8[] = { "utf-8", "utf8", "unicode-1-1-utf-8" };
    for (u32 i = 0; i < sizeof SJ / sizeof *SJ; i++) if (ci_eqn(p, n, SJ[i])) return IF_ENC_SJIS;
    for (u32 i = 0; i < sizeof EJ / sizeof *EJ; i++) if (ci_eqn(p, n, EJ[i])) return IF_ENC_EUCJP;
    for (u32 i = 0; i < sizeof U8 / sizeof *U8; i++) if (ci_eqn(p, n, U8[i])) return IF_ENC_UTF8;
    return IF_ENC_UTF8; /* 未知ラベル = 安全側フォールバック（曖昧解釈しない。docs 凍結） */
}

/* "charset" の直後の値を取り出す（属性形 / content 形の両方を1スキャナで拾う）。
 * 直前が名前構成文字([A-Za-z0-9_-])の "charset"（data-charset 等）は拾わない。
 * 値は引用符付き/裸形を受理。見つかれば out(cap) に終端付きで写して true。 */
static bool extract_charset(const u8 *p, u32 n, char *out, u32 cap) {
    static const char KW[] = "charset";
    for (u32 i = 0; i + 7 <= n; i++) {
        if (ci_lo(p[i]) != 'c') continue;
        bool hit = true;
        for (u32 k = 0; k < 7; k++)
            if (ci_lo(p[i + k]) != (u8)KW[k]) { hit = false; break; }
        if (!hit) continue;
        if (i && (ci_lo(p[i - 1]) == '-' || ci_lo(p[i - 1]) == '_' ||
                  (p[i - 1] >= '0' && p[i - 1] <= '9') ||
                  (ci_lo(p[i - 1]) >= 'a' && ci_lo(p[i - 1]) <= 'z')))
            continue; /* xxxcharset / data-charset は別語 */
        u32 j = i + 7;
        while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
        if (j >= n || p[j] != '=') continue;
        j++;
        while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
        u8 q = 0;
        if (j < n && (p[j] == '"' || p[j] == '\'')) q = p[j++];
        u32 b = j;
        while (j < n) {
            u8 c = p[j];
            if (q) { if (c == q) break; }
            else if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == ';' ||
                     c == '/' || c == '>' || c < 0x20) break;
            j++;
        }
        u32 len = j - b;
        if (!len || len >= cap) return false;
        memcpy(out, p + b, len);
        out[len] = 0;
        return true;
    }
    return false;
}

IfEnc if_charset_from_http(IfStr ctype_header) {
    if (!ctype_header.p || !ctype_header.n) return IF_ENC_UTF8;
    char lab[64];
    if (!extract_charset((const u8 *)ctype_header.p, ctype_header.n, lab, sizeof lab))
        return IF_ENC_UTF8;
    return if_charset_label(lab, (u32)strlen(lab));
}

/* meta prescan: 先頭 limit バイト内の <meta ...> 要素から charset 値を拾う
 * （WHATWG 12.2.2.1 の単純形。<meta の後続が空白/スラッシュ/'> 'のときだけタグと認める） */
static bool prescan_meta(const u8 *p, u32 n, char *out, u32 cap) {
    u32 limit = n > 4096u ? 4096u : n;
    for (u32 i = 0; i + 6 <= limit; i++) {
        if (p[i] != '<') continue;
        if (ci_lo(p[i + 1]) != 'm' || ci_lo(p[i + 2]) != 'e' ||
            ci_lo(p[i + 3]) != 't' || ci_lo(p[i + 4]) != 'a') continue;
        u8 c5 = p[i + 5];
        if (!(c5 == ' ' || c5 == '\t' || c5 == '/' || c5 == '>' || c5 == '\n' || c5 == '\r'))
            continue;
        u32 j = i + 5;
        while (j < limit && p[j] != '>') j++;
        if (j >= limit) return false; /* タグ未完 = 以降にタグなしと同義（継続しない） */
        if (extract_charset(p + i + 5, j - (i + 5), out, cap)) return true;
        i = j; /* この meta に charset が無ければ次の < から再開 */
    }
    return false;
}

IfEnc if_charset_sniff(IfStr ctype_header, IfStr bytes, bool *out_utf8_bom) {
    bool bom = bytes.n >= 3 && (const u8 *)bytes.p &&
               (u8)bytes.p[0] == 0xEF && (u8)bytes.p[1] == 0xBB && (u8)bytes.p[2] == 0xBF;
    if (out_utf8_bom) *out_utf8_bom = bom;
    /* 1) HTTP Content-Type（対応ラベルに確定したときのみ確定） */
    if (ctype_header.p && ctype_header.n) {
        char lab[64];
        if (extract_charset((const u8 *)ctype_header.p, ctype_header.n, lab, sizeof lab)) {
            IfEnc e = if_charset_label(lab, (u32)strlen(lab));
            if (e != IF_ENC_UTF8 ||
                ci_eqn(lab, (u32)strlen(lab), "utf-8") || ci_eqn(lab, (u32)strlen(lab), "utf8"))
                return e;
        }
    }
    /* 2) UTF-8 BOM */
    if (bom) return IF_ENC_UTF8;
    /* 3) meta prescan */
    if (bytes.p) {
        char lab[64];
        if (prescan_meta((const u8 *)bytes.p, bytes.n, lab, sizeof lab))
            return if_charset_label(lab, (u32)strlen(lab));
    }
    return IF_ENC_UTF8;
}

/* ---- 復号本体 ---- */

static u32 emit_utf8(u8 *o, u32 cp) { /* BMP のみ（表は BMP 保証。FFFD=0xFFFD も BMP） */
    if (cp < 0x80) { o[0] = (u8)cp; return 1; }
    if (cp < 0x800) { o[0] = (u8)(0xC0 | (cp >> 6)); o[1] = (u8)(0x80 | (cp & 63)); return 2; }
    o[0] = (u8)(0xE0 | (cp >> 12)); o[1] = (u8)(0x80 | ((cp >> 6) & 63));
    o[2] = (u8)(0x80 | (cp & 63)); return 3;
}

static u16 jis_idx(const u16 *tbl, u32 idx) { return idx < IF_JIS_CELLS ? tbl[idx] : 0; }

static u16 sjis_ext_get(u16 key) { /* 昇順二分探索 */
    u32 lo = 0, hi = IF_SJIS_EXT_N;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        u16 k = (u16)(if_sjis_ext_tbl[mid] >> 16);
        if (k < key) lo = mid + 1;
        else if (k > key) hi = mid;
        else return (u16)(if_sjis_ext_tbl[mid] & 0xFFFFu);
    }
    return 0;
}

IfStr if_charset_decode(IfArena *a, IfStr in, IfEnc enc) {
    const u8 *s = (const u8 *)in.p;
    u32 n = in.n;
    /* 上限膨張は 1B→3B（半角カナ / 孤立バイトの FFFD）。3n+3 で全経路を覆う */
    u64 cap = (u64)n * 3u + 3u;
    u8 *o = (u8 *)if_arena_alloc(a, cap);
    u64 on = 0;
    u32 i = 0;
    if (enc == IF_ENC_SJIS) {
        while (i < n) {
            u8 b = s[i];
            if (b < 0x80) { o[on++] = b; i++; continue; }
            if (b >= 0xA1 && b <= 0xDF) { /* 半角カナ U+FF61+(b-0xA1) */
                on += emit_utf8(o + on, 0xFF61u + (u32)(b - 0xA1)); i++; continue;
            }
            if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
                if (i + 1 >= n) { on += emit_utf8(o + on, 0xFFFD); i++; continue; }
                u8 t = s[i + 1];
                if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC)) {
                    u16 cp = sjis_ext_get((u16)(((u16)b << 8) | t));
                    if (!cp) { /* tbl_jis208 を WHATWG pointer で引く */
                        u32 trail_off = t < 0x7F ? 0x40u : 0x41u;
                        u32 lead_off = b < 0xA0 ? 0x81u : 0xC1u;
                        u32 idx = (u32)(b - lead_off) * 188u + (u32)(t - trail_off);
                        cp = jis_idx(if_jis208_tbl, idx);
                    }
                    on += emit_utf8(o + on, cp ? cp : 0xFFFD);
                    i += 2;
                } else { /* trail 不成立 → FFFD は lead のみ消費（trail は restore） */
                    on += emit_utf8(o + on, 0xFFFD);
                    i++;
                }
                continue;
            }
            on += emit_utf8(o + on, 0xFFFD); i++; /* 0x80/0xA0/0xFD.. 孤立 */
        }
    } else { /* IF_ENC_EUCJP */
        while (i < n) {
            u8 b = s[i];
            if (b < 0x80) { o[on++] = b; i++; continue; }
            if (b == 0x8E) { /* SS2: 半角カナ */
                if (i + 1 >= n) { on += emit_utf8(o + on, 0xFFFD); i++; continue; }
                u8 t = s[i + 1];
                if (t >= 0xA1 && t <= 0xDF) {
                    on += emit_utf8(o + on, 0xFF61u + (u32)(t - 0xA1)); i += 2;
                } else { on += emit_utf8(o + on, 0xFFFD); i++; } /* t restore */
                continue;
            }
            if (b == 0x8F) { /* SS3: JIS X 0212（trail は 0xA1..0xFE。0xFF は行越境不可） */
                if (i + 1 >= n) { on += emit_utf8(o + on, 0xFFFD); i++; continue; }
                u8 t = s[i + 1];
                if (t < 0xA1 || t > 0xFE) { on += emit_utf8(o + on, 0xFFFD); i++; continue; } /* t restore */
                if (i + 2 >= n) { on += emit_utf8(o + on, 0xFFFD); i += 2; continue; }
                u8 u = s[i + 2];
                if (u < 0xA1 || u > 0xFE) { on += emit_utf8(o + on, 0xFFFD); i += 2; continue; } /* u restore */
                u16 cp = jis_idx(if_jis212_tbl, (u32)(t - 0xA1) * 94u + (u32)(u - 0xA1));
                on += emit_utf8(o + on, cp ? cp : 0xFFFD);
                i += 3;
                continue;
            }
            if (b >= 0xA1 && b <= 0xFE) { /* JIS X 0208 面（lead/trail 共に 0xA1..0xFE） */
                if (i + 1 >= n) { on += emit_utf8(o + on, 0xFFFD); i++; continue; }
                u8 t = s[i + 1];
                if (t >= 0xA1 && t <= 0xFE) {
                    u16 cp = jis_idx(if_jis208_tbl, (u32)(b - 0xA1) * 94u + (u32)(t - 0xA1));
                    on += emit_utf8(o + on, cp ? cp : 0xFFFD);
                    i += 2;
                } else { on += emit_utf8(o + on, 0xFFFD); i++; } /* t restore */
                continue;
            }
            on += emit_utf8(o + on, 0xFFFD); i++; /* 0x81..0x8D,0x90..0x9F */
        }
    }
    return if_str((const char *)o, (u32)on);
}
