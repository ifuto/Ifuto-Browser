//! Ifuto Browser 本体（HTML パーサ / DOM / CSS / レイアウト / 文字コード層 等）の
//! Rust 移行コア。
//!
//! フェーズ 0〜6 で JS エンジン（Aklus）を C から Rust へ移行した。本クレートは
//! その「ブラウザ本体」側（`src/*.c` のうち `src/akl/` を除く約 15k 行）の移行を
//! 担う。方針は Aklus 移行と同一:
//!
//! - **1 モジュール = 1 検証ゲート**。C 実装を回帰オラクルに並走させ、入出力を
//!   突合してから差し替える（「黙って違う結果を返さない」）。
//! - `#![forbid(unsafe_code)]` を維持。メモリ安全性・アラインメント・範囲検査は
//!   型システムに委ね、C の手動管理（手動 free / ポインタ算術 / 終端検査漏れ）を
//!   構造的に排除する。
//!
//! # 移行順（依存が少ない純粋な葉モジュールから）
//!
//! | C モジュール | Rust モジュール | 状態 |
//! |---|--- |--- |
//! | `src/utf8.c` | [`utf8`] | 完了 |
//! | `src/strutil.h`（スライス補助） | [`strutil`] | 完了 |
//! | `src/charset.c` | （未着手） | |
//! | `src/arena.c` | （未着手） | |
//! | `src/html_tok.c` / `html_tree.c` | （未着手） | |
//! | ... | | |
//!
//! # 検証オラクル
//!
//! C 側の単体テスト（`tests/test_utf8.c` / `tests/test_charset.c` 等）と、総合ゲート
//! （`make test` 625,125 checks / html5lib 1922/1922 / golden）を回帰オラクルとして
//! 使い続ける。各 Rust モジュールは C テストと同一の期待値を Rust テストとして
//! 再現し、加えて全数走査（`band_w2` の 3 バイト全探索等）で表外セルの安全性を
//! 機械的に証明する。

#![forbid(unsafe_code)]
#![warn(missing_docs)]

/// 共通型・ハードリミット（C の `common.h` 相当）。
pub mod common;

/// ゼロコピー文字列スライス補助（C の `strutil.h` 相当）。
pub mod strutil;

/// UTF-8 デコーダ / エンコーダ / セル幅（C の `utf8.c` 相当）。
pub mod utf8;

/// 文字コード層（Shift_JIS / EUC-JP → UTF-8。C の `charset.c` 相当）。
pub mod charset;

/// 文字コード変換表（`tools/gen_charset.py` 生成。C の `charset_tables_gen.h` 相当）。
pub mod charset_tables;

/// 拡張 manifest パーサ（C の `ext_manifest.c` 相当。純粋関数のみ）。
pub mod ext_manifest;

/// HTML 名前付き文字参照の解決（C の `entities_gen.h` + `html_tok.c` の検索プリミティブ）。
pub mod entities;

/// HTML 名前付き文字参照の生成表（`tools/gen_entities.py` 生成）。
pub mod entities_tables;

/// 永続ストア層の読み面パーサ（C の `store.c` の session / bookmarks 相当）。
pub mod store;

/// HTML タグ表（C の `dom.c` の `IF_TAGS` + 検索プリミティブ相当）。
pub mod tags;

/// HTML タグ表の生成データ（`tools/gen_tags.py` 生成。正本は `src/dom.c`）。
pub mod tags_tables;

/// HTML トークナイザ（C の `html_tok.c` 相当）。
pub mod html_tok;

/// DOM ノードモデル（C の `dom.c` / `dom.h` 相当のデータ構造と純粋ヘルパ）。
pub mod dom;

/// HTML ツリービルダ（C の `html_tree.c` 相当。WHATWG insertion modes）。
pub mod html_tree;

/// CSS サブセット（パース + カスケード。C の `css.c` 相当）。
pub mod css;

/// レイアウト（整数セル座標系。C の `layout.c` 相当）。
pub mod layout;

/// セルグリッドレンダラ（C の `render_ansi.c` 相当）。
pub mod render;

/// Markdown 変換層（C の `md.c` の文字列 backend 相当）。
pub mod md;

/// 軽量画像デコード（BMP / PNG。C の `image.c` 相当）。
pub mod image;
