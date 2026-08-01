/* Ifuto GUI — 唯一の UI フロントエンド（raw X11 + ソフトラスタ）。
 * 2026-08-01 決定: TUI は完全廃止。UI は GUI 一本（対話は GUI、検証は --shot の
 * ヘッドレス PPM パイプライン。端末向けテキスト出力は --no-ansi 等の CLI render が担う）。
 * 依存は libc/libm のみを維持（ldd: linux-vdso/libc/ld (+libm)）。 */
#ifndef IFUTO_GUI_H
#define IFUTO_GUI_H

/* X サーバに接続して対話 GUI を走らせる。終了コードは従来の gui main と同じ
 * （0=正常, 2=X 接続/窓生成の失敗）。initial_path は NULL 可。 */
int if_gui_run(const char *initial_path);

/* ヘッドレス検証: X なしで対話時と同一のラスタパイプラインを走らせ、
 * フルページ PPM を out_ppm に書く（gui_smoke が画素監査する経路）。
 * 0=成功, 1=出力失敗。 */
int if_gui_shot(const char *input_path, const char *out_ppm);

#endif
