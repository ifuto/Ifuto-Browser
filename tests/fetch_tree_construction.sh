#!/usr/bin/env bash
# WPT tree-construction テストデータのベンダー取得（ピン留め・再現可能）。
# 出典: web-platform-tests/wpt html/syntax/parsing/resources/*.dat
# ライセンス: WPT は BSD-3-Clause（同梱の LICENSE.wpt を参照）。
# 注: raw.githubusercontent.com が届かない環境があるため gh api (raw accept) 経由で取る。
set -eu

SHA="0acb81f2619df3096926dcc5a9d234d668f5a38e"  # wpt master HEAD as of 2026-07-28
BASE="repos/web-platform-tests/wpt/contents/html/syntax/parsing/resources"
DEST="$(dirname "$0")/wpt-tree-construction"
mkdir -p "$DEST"

FILES="adoption01.dat adoption02.dat blocks.dat comments01.dat doctype01.dat domjs-unsafe.dat \
entities01.dat entities02.dat foreign-fragment.dat html5test-com.dat inbody01.dat isindex.dat \
main-element.dat math.dat menuitem-element.dat namespace-sensitivity.dat noscript01.dat \
pending-spec-changes-plain-text-unsafe.dat pending-spec-changes.dat plain-text-unsafe.dat \
processing-instructions.dat quirks01.dat ruby.dat scriptdata01.dat scripted_adoption01.dat \
scripted_ark.dat scripted_webkit01.dat search-element.dat svg.dat tables01.dat template.dat \
tests1.dat tests2.dat tests3.dat tests4.dat tests5.dat tests6.dat tests7.dat tests8.dat \
tests9.dat tests10.dat tests11.dat tests12.dat tests14.dat tests15.dat tests16.dat tests17.dat \
tests18.dat tests19.dat tests20.dat tests21.dat tests22.dat tests23.dat tests24.dat tests25.dat \
tests26.dat tests_innerHTML_1.dat tricky01.dat void-in-phrasing.dat webkit01.dat webkit02.dat"

for f in $FILES; do
    gh api "${BASE}/${f}?ref=${SHA}" -H "Accept: application/vnd.github.raw" > "$DEST/$f"
done
gh api "repos/web-platform-tests/wpt/contents/LICENSE.md?ref=${SHA}" \
    -H "Accept: application/vnd.github.raw" > "$DEST/LICENSE.wpt"
echo "${SHA}" > "$DEST/PINNED.sha"
echo "fetched $(ls "$DEST"/*.dat | wc -l) dat files at ${SHA}"
