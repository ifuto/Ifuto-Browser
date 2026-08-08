# Ifuto Browser — 文字コード層 凍結正本（v0.3 A1: S/A 完遂プログラム）

本書は src/charset.[ch]・tools/gen_charset.py・src/charset_tables_gen.h の仕様凍結である。
変更は本書の改訂と同時でなければ成果に数えない（検証なき変更禁止の法則）。

## 1. 責務
HTML 経路の入力（http / file / stdin）を **UTF-8 へ正規化**してから単一 WHATWG
パーサへ渡す（多層防御不変: パーサは UTF-8 のみを見る）。対象は Shift_JIS 系と
EUC-JP 系 —— 日本語 Web の実効文字コード 2 系統に限定する意図的線引き。

- **対象外（凍結）**: MD 経路（UTF-8 凍結が md 正本仕様）、ifuto:// 内部ページ
  （generated UTF-8）、UTF-16 系（BOM があっても非対応 = UTF-8 扱い。将来課題）。
- **ゼロコスト不変条件**: UTF-8 確定入力では出力は入力の恒等切片（BOM 剥がしは
  p+=3 のみ）。新規確保・走査は non-UTF-8 確定時に限る。16MB md ミッション計測
  経路には関門が存在しない（`if (!md_doc)` ゲート。実測 122.23ms ∈ 帯内）。

## 2. 判定優先順位（if_charset_sniff）
1. HTTP `Content-Type` の charset（対応ラベルに確定したときのみ。未知ラベルは
   「ヘッダ不成立」として次段へ落とす —— ゴミ宣言で meta を殺さないための規則）
2. UTF-8 BOM（EF BB BF。検出は out_utf8_bom で報告、剥がすのは関門側の責務）
3. meta prescan（先頭 4096 バイト。`<meta` 後続ゲート=空白/`\t`/`\n`/`\r`/`/`/`>`。
   属性形 `charset=` と http-equiv の content 値内 `charset=` を単一スキャナで拾う。
   `data-charset` 等、直前が名前構成文字のものは別語として拾わない）
4. 既定 UTF-8

## 3. ラベル対応（ci・前後空白許容）
- Shift_JIS 系: `shift_jis shift-jis sjis csshiftjis ms_kanji windows-31j x-sjis
  cp932 ms932 x-ms-cp932` → IF_ENC_SJIS
- EUC-JP 系: `euc-jp cseucpkdfmtjapanese x-euc-jp` → IF_ENC_EUCJP
- UTF-8 系: `utf-8 utf8 unicode-1-1-utf-8` → IF_ENC_UTF8
- **未知ラベルは IF_ENC_UTF8 へ安全側フォールバック**（WHATWG なら windows-1252
  等に落ちるところを、本実装は曖昧解釈を持ち込まない原則で UTF-8 に倒す。
  iso-8859-1 系ページは化ける —— 正直な既知犠牲として明記。将来課題）。

## 4. 変換表（src/charset_tables_gen.h = 機械生成・手編集禁止）
- 正本は **python codec**（euc_jp / shift_jis / cp932）。`tools/gen_charset.py` が
  全セルを codec から引き、strict shift_jis ↔ euc_jp の 0208 面合意を交叉検証
  （不一致が 1 セルでもあれば生成失敗）。oracle 21 件目として再生成一致を凍結。
- `tbl_jis208[8836]`: (ku−1)*94+(ten−1) → Unicode BMP（無効 0→U+FFFD）。
  SJIS 側は WHATWG pointer 公式で idx を計算、EUC 側は (b−0xA1) 直索引で共有。
- `tbl_jis212[8836]`: EUC SS3（0x8F）の JIS X 0212 面。
- `tbl_sjis_ext[2731]`: cp932 が復号できて strict shift_jis が不可/異義のバイト対
  （NEC 選定・IBM 拡張・下記 6 件）。key 昇順・C 側二分探索で tbl_jis208 に優先。

### 波ダッシュ問題（6 件）の確定方針 = **cp932 採用**
0x8160〜、0x8161∥、0x817C－、0x8191¢、0x8192£、0x81CA￢ を cp932（windows-31j）
の解釈で復号する。実在の日本語ページは windows-31j 実装を前提に書かれており、
実効挙動に従う。unicode.org 厳密系・WHATWG index 系とはこの 6 件で乖離する
（0x8160: U+FF5E / 厳密系 U+301C）。乖離は本節で凍結記録済。生成器が
「override は正確にこの 6 件」を assert している（7 件目が出たら生成失敗）。
- 単バイト半角カナ（SJIS 0xA1-0xDF／EUC SS2 0x8E+0xA1-0xDF）→ U+FF61+ 公式。
- 0x5C はバックスラッシュのまま（円記号への描き換えは renderer の責務ではなく
  行わない。codec 合意済）。

## 5. malformed 消費規則（U+FFFD。表示ずれ/XSS 様すり抜け差分の防止で凍結）
- SJIS: lead 後 EOF=FFFD(1 消費)。trail ∈ {0x40-0x7E,0x80-0xFC} なら対で復号
  （無字セル=FFFD・2 消費）。trail 範囲外=FFFD・**lead のみ消費（trail restore）**。
- EUC: 0x8E/0x8F/0208-lead 同型。復号 trail/lead は共に **0xA1-0xFE**（0xFF は
  受理しない = 区点の行越境エイリアスを構造排除）。途中 EOF は確定済み分まで消費。
- いずれも 1 回の FFFD で最低 1 バイト前进する = 無限ループ不成立。

## 6. 配線
- net.c: IfHttpHead.content_type 追加・if_http_get_ex（redirect 最終応答の値を返す。
  if_http_get は薄い wrapper で後方互換）。
- main.c: read_all が ctype を返し、`to_utf8_html`（唯一の関門）を md 以外に適用。
  BOM 剥がしもここ。fragment 経路も HTML なので同じ関門を通る。
- chrome.c(tab_load): external && !md のみ同関門（復号先は src arena = 取り込み時
  複製の入力として載り、parse 後に即破棄される寿命構造不変。内部ページは通さない）。

## 7. 検証（全て機械）
- tests/test_charset.c（60 checks）: ラベル行列・prescan（4096 境界含む）・波ダッシュ
  cp932・NEC/IBM 拡張・半角カナ・malformed 消費規則・65536 全バイト対総当たり
  （ASan 下・出力長上限・決定性）・E2E（sjis→parse→title）・BOM 剥がし規則。
- oracle 17-20 件目: oracle/sjis.html（cp932 fixture。見出し①・半角カナ・波ダッシュ・
  NEC/IBM 拡張）と oracle/eucjp.html（0212 SS3 含む）の render/dom byte-exact。
- oracle 21 件目: gen_charset.py --verify（表 == python codec 再生成）。
- 副産物修正: --dump-dom が DOCTYPE/COMMENT/PI を union の tag_name 誤読みして
  ゴミ出力していた pre-existing バグ（md oracle のみ凍結で未晒だった）を本機に修正
  = sjis.dom/eucjp.dom オラクルは修正後の正しい値で凍結。

## 8. 既知の未解決（正直台帳）
- UTF-16（BOM 検出はするが復号しない）、ISO-8859 系・UTF-7 等は非対応（UTF-8 扱い）。
- prescan は 4096B 限定・コメント内の擬似 `<meta` を区別しない（WHATWG の naive
  prescan と同型の限界。実害は検出漏れのみで安全側）。
- 自動判定（charset 未宣言の legacy ページのヒューリスティック検出）は未実装。
- GUI 実機での sjis/euc ページ描画は X/Xvfb 不在のため未検証（headless --shot 系は
  oracle/gui_smoke で緑だが、実機完成宣言はしない）。
