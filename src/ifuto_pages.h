/* Ifuto — ifuto:// 内部ページ（普通のブラウザの settings/history 相当）
 *
 * 設計: 内部ページは「ローカル情報を HTML として生成」し、以後は通常 DOM 経路に
 * 乗せる（多層防御 = 共通パーサに統一。内部ページ専用の描画分岐は作らない）。
 * 生成は全てローカル完結（INV-2: ネットワーク経路は構造的に存在しない）。
 *
 * ページ（v0.1）:
 *   ifuto://settings  エンジン/メモリ/セキュリティの状態と切替手段の提示
 *   ifuto://history   履歴一覧（store の history.tsv。最新 N 件、新しい順）
 *   ifuto://memory    タブごとの arena 会計（doc/view の予約バイト、総計）
 *   ifuto://about     バージョン・ビルド構成・一次情報への導線
 */
#ifndef IFUTO_PAGES_H
#define IFUTO_PAGES_H

#include "common.h"
#include "strutil.h"
#include "arena.h"

struct IfChrome; /* 依存は一方向: pages → chrome.h は構造体を読むだけ */

/* url が ifuto:// ページなら true を返し、*out_html に HTML を生成する。
 * 内部ページでなければ false（呼出し側は通常のファイル経路へ）。
 * arena a は呼出し側の文書 arena（HTML はその寿命） */
bool if_ifuto_page(IfArena *a, const char *url, struct IfChrome *c, IfStr *out_html);

#endif
