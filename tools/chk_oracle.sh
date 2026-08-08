#!/bin/sh
# oracle 全件の再生成＋sha256 照合。差異があれば終了コード 1。
# 使い方: chk_oracle.sh [BIN]
set -u
BIN="${1:-./build/ifuto}"
ok=0; ng=0
chk() { # chk <expected-sha> <label> <cmd...>
    exp="$1"; label="$2"; shift 2
    got=$("$@" 2>/dev/null | sha256sum | cut -d' ' -f1)
    if [ "$got" = "$exp" ]; then echo "OK   $label"; ok=$((ok+1));
    else echo "DIFF $label exp=$exp got=$got"; ng=$((ng+1)); fi
}
F=oracle/forged.md   # 正本は tracked（.arena は再起動で消える）
M2=.arena/idm/idm-2mb.md
M16=.arena/idm/idm-16mb.md
chk cce7e686bcd28731dbf14f341d2fcc9cd2e2d16381b06ccf502813fd1f3b61c8 forged.out    "$BIN" --no-ansi "$F"
chk eb7fdc75e339a96156516cc8559c8072b205e1fcafc1c1914c29e8ad3d53c5ba forged.ansi   "$BIN" "$F"
chk b501bebfcfef89e421f99fe6dcd3ffc9a5ca39a028db5ea87ae1d96a05217181 forged.w40    "$BIN" --no-ansi --width 40 "$F"
chk 8c624e83ca99a57bee92dd87573f4a9264352310e2ff2a6ed08acc3a3316840d forged.w160   "$BIN" --no-ansi --width 160 "$F"
chk 063da21263a078b9f9329eb68b169ade9216803ea76812f6c121709a8f3f13af forged.dom    "$BIN" --dump-dom "$F"
chk d011c95a98e3b757ab3e94a7fa6b8d1bbd1fcb38dc9a6f4fc87229c830e37a2f forged.links  "$BIN" --no-ansi --links "$F"
chk c092c35dcdcc1c1c17379748fb418c3d303ec65c2345152a54cee20fa0230116 idm-2mb.out   "$BIN" --no-ansi "$M2"
chk d11680089da0fc6e58308fa19622865b19919e261b9caafa484baf4bc7321ab4 idm-16mb.out  "$BIN" --no-ansi "$M16"
chk ba03f18e5e1bedfd0c5b054be798a32fb888e3e91bdc4e1dd48f8e1298c8e9a0 idm-2mb.w40  "$BIN" --no-ansi --width 40 "$M2"
chk 6b022fcd2290d24f718ec11294f1c75b1a99531443f268f3e36c6d7a59670cb4 idm-2mb.w160 "$BIN" --no-ansi --width 160 "$M2"
chk 31036ced7d7cc96e70be2679995c1d51c939ecb684a24f8c4cc0e9d8bc086840 idm-2mb.ansi "$BIN" "$M2"
chk 564f2ee9d421a88e34b7b8d69bbba058a4047d7ef3f67eb9064feb6fa77135f0 idm-2mb.dom  "$BIN" --dump-dom "$M2"
chk 1186bf92885b8fe6281a537dd2a96e82efd455ecd79919b3f9ec2054920ab9c2 forged.styles "$BIN" --dump-styles "$F"
# serial（IF_MD_PAR=0）≡ sliced（既定 2-slice 並列）の差分オラクル（expected は idm-2mb.out と同値への一致が機械意味）
chk c092c35dcdcc1c1c17379748fb418c3d303ec65c2345152a54cee20fa0230116 idm-2mb.serial env IF_MD_PAR=0 "$BIN" --no-ansi "$M2"
# script 実行配線の E2E byte-exact オラクル（v0.3。mutation が描画に出る / IF_SCRIPT=0 で出ない、の双方向）
S=oracle/script.html
chk 13655f7074599cfe39fcb756bd3f02b44b92125087f35286340ff957e2f5291b script.out    "$BIN" --no-ansi "$S"
chk afb095effd08167731a4d3aa41924cd7bcdc9bd20ccf97847375a2b80ee4be5e script.killed env IF_SCRIPT=0 "$BIN" --no-ansi "$S"
# 文字コード層（v0.3 A1）: Shift_JIS / EUC-JP の E2E byte-exact（meta charset 検出 → UTF-8 正規化
# → 描画/DOM。波ダッシュ cp932 採用・NEC/IBM 拡張・SS3 0212・半角カナを fixture に内包。
# sjis.dom/eucjp.dom は --dump-dom の DOCTYPE/COMMENT 表示修正（v0.3）後の値で凍結）
chk 9dd9246fa8886c99b193910d02f216e4416f8d78f3b3ae99eb3decd5aa30e69c sjis.out    "$BIN" --no-ansi oracle/sjis.html
chk a07a5cb171c0f6f44dc0355dbf1ed62866e825f82d234c09c1988ef994b0eced sjis.dom    "$BIN" --dump-dom oracle/sjis.html
chk d8340987f8314299840399b6dc7d28b997b0483306721a6125909c3e65768abe eucjp.out   "$BIN" --no-ansi oracle/eucjp.html
chk 8042b383ceca03aff92ab25799b8dcf16646fc38de4d93425a826f5979b4113d eucjp.dom   "$BIN" --dump-dom oracle/eucjp.html
# 変換表の再生成一致オラクル（python codec が正本。表の手編集・退行を機械封殺）
if python3 tools/gen_charset.py --verify >/dev/null; then echo "OK   charset.tables"; ok=$((ok+1));
else echo "DIFF charset.tables"; ng=$((ng+1)); fi
echo "oracle: ok=$ok ng=$ng"
[ "$ng" -eq 0 ]
