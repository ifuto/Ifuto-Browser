# ARCHITECTURE.md — Ifuto Browser

「史上最強の軽量ブラウザ」を**考えなしの全部入りではなく、効用関数の最大化**として建てる。
この文書は設計の固定点・不変条件・明示した近似・残存攻撃面の唯一の真実源（single source of truth）である。

```
入力（untrusted なバイト列。常に敵意を仮定）
  → UTF-8 デコード（FFFD 回復）          src/utf8.c
  → HTML トークナイザ                    src/html_tok.c
  → ツリービルダ → DOM                   src/html_tree.c, src/dom.c
  → CSS パース → カスケード              src/css.c（RuleSet 索引 = Blink 戦略、23.32×）
  → レイアウト（整数セル座標）           src/layout.c
  → ペイント（セルグリッド・ソフトラスタ）src/render_ansi.c
  → 発行（ANSI/プレーン）・CLI           src/main.c
```

## 0. 優先度台帳（2026-08-08 ユーザー確定・この順に全て本気で）

1. **JS のすべての構文に対応させる**（2026-08-08 追加・最優先。akl を「サブセット」から
   「実用的 JS」へ。達成基準: 実サイトのスクリプトが SyntaxError なしで通る）
2. 省メモリ性 / FPS 向上性
3. 省 CPU / 省 GPU / 起動速度等速度全般 / WPT の高さ（99% 前後が目標 → tree-construction 100% 達成済）
4. セキュリティ / クラッシュ耐性
5. 拡張機能の作りやすさ・導入のしやすさ・安全性
6. Web 開発者ツール

## 1. 効用関数とウェイト（意思決定基準）

`w_footprint = w_correctness = 0.20` を双頭とし、`w_security = 0.16`。
速度・互換性・足場の軽さのトレードオフは列で示す:

| 選択 | 採用 | 棄却 | 根拠 |
|---|---|---|---|
| エンジン | 全部自作（C11） | Chromium 系・litehtml・WebView | ユーザ指示 + フットプリント支配 |
| メモリ | ページ単位 arena | per-object malloc/free + 所有権地獄 | dangling を構造的に不可能にする |
| プロセス | 単一プロセス | サイト分離マルチプロセス | v0.1 の攻撃面最小化のため。v0.5 で見直す（§6） |
| GPU | バックエンド境界 + ソフトラスタ先行 | いきなり Vulkan/D3D12/Metal | この Linux コンテナで検証不能なコードは書かない |
| JS | Akl（自作、C11・JIT なし、v0.5 台帳行） | QuickJS 埋込・JIT 系 | 攻撃面とメモリを構造排除（E1 で拡張実行として本体巻取済） |

## 2. 不変条件（コードに課された法）

1. **パーサは止まらない**: あらゆる経路でカーソルは単調前進。無限ループは構造的に禁止。
2. **arena ポインタはページ寿命**: 個別 free なし、dangling なし。OOM/異常サイズは `if_fatal` で即死（黙った破壊より fail-fast）。
3. **型レベルで無効状態を排除**: IfLen の unit 列挙、IfStyle の display 列挙。文字列比較で状態を表さない。
4. **untrusted 入力の上限**: `IF_MAX_INPUT_BYTES`（64MB）、`IF_MAX_DOM_NODES`、スタック深さ上限。
5. **style 未適用でも layout/render は落ちない**（`IF_STYLE_FALLBACK`）。
6. **測定の捏造禁止**: ベンチはハーネス付き・方法は BENCH.md に明記。壊れた計測は報告しない。

## 3. 明示した近似（嘘をつかないための台帳）

| 近似 | 影響 | 脱却計画 |
|---|---|---|
| レイアウトは整数セル座標（8px×16px グリッド） | 端末では正確。Sub-px 表現不能 | px 精度レイアウトを GPU 化と同時に別経路で追加（IfBox API は維持） |
| 縦マージン相殺は兄弟間のみ | 親子貫通ケースで ±1 行 | v0.2 |
| マーカーは親 padding 帯に描画（2〜4 セル） | `list-style-position: inside` 未対応 | v0.2 |
| テーブルは display:block として積む。ツリー側は **table 挿入モード群 + foster parenting 実装済**（in-table/caption/colgroup/table-body/row/cell + pending table text） | レイアウトはまだセル幅概念なし | テーブルレイアウト（v0.2 後半） |
| ~~`<title>`/`<textarea>` は RAWTEXT~~ → **RCDATA 化済み**（文字参照デコード + textarea 先頭 LF 規則） | 解消 | 済 |
| `display:inline-block` → inline、table 系 display → block | 一部サイトで組版差 | v0.3+ |
| ~~コメント/doctype は DOM に残さない~~ → **保持に変更**（PI `<?target data?>` も PI として区別） | 解消 | 済 |
| foreign content（SVG/MathML） | 実装済み: ns 入域/復帰・integration points・breakout 完全表（listing/menu/meta/nobr/ruby 含む）・case 調整・CDATA・serializer prefix・foreign `</p>`/`</br>` の HTML 再処理規則 | 解消（WPT foreign-fragment 66 件で網羅） |
| template は **content DocumentFragment 分離済み** + **"in template" 挿入モード実装済**（table トークン routing・frame 無視・EOF の template pop。現行世代の template.dat も全合格） | 解消 | 済 |
| fragment 解析（innerHTML 相当、WHATWG 13.4）も **実装済**: 仮想 html root + 仮想 context 要素（adjusted current node）+ reset appropriately の fragment 基底 + raw/rcdata の "appropriate end tag 非合致" 規則 | skip は `#script-on` 12 件のみ（scripting UA 前提・恒久） | 済（`--fragment CTX`、1922/1922 = 100.0%） |
| 奇行系: quirks 判定・foster parenting・厳密 adoption agency・foreign breakout 完全表・全実体参照 2,125 名（全て実装済） | 壊れた HTML でのツリー差異ゼロ | **tree-construction 100.0%**（実行可能 1,922 件全合格、残りは script-on 12 のみ） |
| 入力 CR/CRLF → LF 正規化なし | `\r` 含む入力でツリー差異の可能性 | v0.2 継続（入力前処理に正規化層） |
| isindex/body↔head 移動等の非推奨・特殊規則 | 全合格（isindex.dat 4/4 含む全件） | 解消 |
| フォント: CLI は SGR 属性（太字/斜体/下線/打消）、GUI は自前ピクセルフォント + 太字の横オフセット重ね + 斜体の oblique shear（fb.c）。等幅 | 真ベクターフォント/AA なし | ソフトラスタ品質の段階改善（要検証設計） |
| GUI: 生 X11 プロトコル単一 UI（Xlib/XCB 不リンク、TUI は廃止）、自前 5x7+16x16 フォント（かな全量・CJK 第 5 陣まで 103 kanji）、タブ/omnibox/status・リンククリック・hover・セッション永続化（原子書込・遅延復元）・`--shot` 決定ラスタ検証（51 checks）。ldd は vdso/libm/libc/ld のみ | 台帳（続課題）: 漢字第 6 陣以降の補完、anchor 遷移、斜体傾斜 | CJK フォント段階投入・akl DOM バインド拡張（v0.4） |
| `<script>` akl 実行（v0.3。src/script.c、正本 docs/SCRIPTING.md）: 文書順・1 頁 1 AklRT・style 適用前・失敗は script 単位で隔離。最小 DOM バインド（`document.title`/`body`/`documentElement`/`getElementById`、要素 `.textContent`/`.id`/`.tagName`）は `AklHandleVTab`（ptr=DOM arena 所有・script RT 先に破棄の構造証明）。`has_script` parse 観測スイッチで非含有文書は走査ゼロ | 外部 src 取得・イベント・querySelector は v1 非対象（明白スキップ/拒否で数える）。module は v0.5 で inline 実行対応（import 解決はローダ未装備のため明白失敗） | DOM バインド拡張（v0.4）、src 取得（net.c と連結） |
| slim-DOM（法則「画面描画に関係ないものは DOM しない」）: **template 配下**を DOM 非構築（script は v0.3 で実行対象のため本文ごと残す = 法則の正しい適用。renderer は従来機構で script subtree を描画しない）。**style は cascade が本文を読むので剃らない**。構築状態機械は完全実行、root は marker 残置。**入力 compaction（v0.3 本丸）は取り込み時複製で実装済**: GUI のみ `if_dom_copy_strings` で切片の誕生点複製→一時入力 arena 即破棄（walk 型は +62ms 実測棄却。ΔVmHWM −3.4MB/16MB 文書・50 タブ換算 −170MB。BENCH.md 台帳） | 剃りの単タブ効用は従来通り限定（ノードヘッダ+走査回避。IfNode 80B→69B（2026-08-10: IfStr 12B packed + kind u8 + ノード 8B アライン経路。16MB md で −18MB）
| レンダ grid は viewport 窓のみ materialize（`IfGrid.y_off`、GUI/CLI 共用ビルダ） | 窓再構築はスクロール/リサイズ/タブ切替ごとに O(boxes) クリップ走査 | 差分ペイント（v0.3 候補） |
| Markdown 表示: `.md` は md.c が多層防御つき HTML に変換して共通パイプラインへ（生 HTML 素通し禁止） | MD の入れ子深度上限で飽和（quote 8 / list 16） | MD 全構文網羅（v0.3 継続） |
| 画像デコードは **実装済**（v0.4: src/image.c。PNG（8bit RGBA/RGB/グレー±α・全フィルタ・zlib inflate 内蔵）と BMP（24/32bpp）。`--imgdecode FILE` で PPM 出力。img タグへの統合は次） | パレット/インターレース/16bit PNG は明白拒否 | img タグ描画統合 |
| HTTP/1.1 取得は **実装済**（src/net.c、防御的 4 パーサ + fuzz_net。plaintext のみ） | — | 済 |
| HTTPS/TLS は **実装済**（v0.3: BearSSL 静的リンク。TLS 1.2・ECDHE-RSA/AES-GCM・
  システム CA バンドル（`IFUTO_CA_BUNDLE` で override）をトラストアンカーに
  x509 チェーン検証 + サーバ名（SAN/CN）照合。src/tls.c。ローカル CA + openssl
  s_server による E2E は tests/tls_smoke.sh が機械検証。実サーバへの接続はこの
  headless コンテナの egress が TLS ハンドシェイクを遮断するため検証対象外と明記） | BearSSL の既知制限: IP 直打ち URL の SAN 照合不可（DNS 名のみ）。TLS 1.3 は
  BearSSL の上限外（サーバが 1.3 のみなら接続不能）。セッション再開・
  クライアント証明書・SNI 以外の拡張は未使用 | BearSSL のアップストリーム追随 |
| 画像デコードは **実装済**（v0.4: src/image.c。PNG（8bit RGBA/RGB/グレー±α・全フィルタ・zlib inflate 内蔵）と BMP（24/32bpp）。`--imgdecode FILE` で PPM 出力。img タグへの統合は次） | パレット/インターレース/16bit PNG は明白拒否 | img タグ描画統合 |

## 4. セキュリティモデル（防御的。現状の残存攻撃面を隠さない）

**信頼境界**: 外部コンテンツ → エンジン → 端末エスケープ出力。

- パーサ/レイアウト/レンダラは全経路 ASan+UBSan+stack-protector+fuzz 常時（2026-08-08
  時点で UBSan runtime note は 0 を確認。layout.c のシャード結合で検出していた
  `memcpy(NULL,0)` 4 系統は n>0 ガードで解消・run_tests 出力はクリーンに回復。
  新規出現は run_tests 出力で即座に止める）。
- **現状の残存攻撃面（正直に）**:
  - 出力端末への制御シーケンス混入: HTML 中の ESC 等の制御文字はグリフ幅 0 として**描画せず捨てる**が、
    デコード済みの C0/C1 が `put_cp` 経路に入らないことを fuzz で継続確認が必要（要テスト追加）。
  - DoS: 巨大/深い文書はカウンタ上限で打ち切る。`IF_MAX_DOM_NODES` 超過で切断（描画は継続）。
  - 単一プロセスのため任意コード実行まで至るバグは即ユーザ端末。だからこそ fuzz・sanitizer を文化にする。
  - CSS セレクタのバックトラック爆発: depth 4096 上限内だが、悪意あるネスト `div div div … {}` は
    要素×深さで増える。サイト分離移行時に per-セレクタ評価上限を入れる（ROADMAP）。
- **やらないこと**: 自作 TLS、自作暗号、外部コードの盲目的取り込み、exploit の開発。

## 5. バックエンド境界（GPU への脱却路）

```
layout（座標系は差し替え可能）──→ [backend 境界: 矩形/文字/罫線の描画プリミティブ列]
                                     ├─ v0.1: セルグリッド（ソフト）render_ansi.c  ← 現在ここ
                                     ├─ v0.3: ソフトピクセルラスタ（フォントラスタライズ込み）
                                     ├─ v0.3: Vulkan（Linux で実測可能になってから）
                                     ├─ v0.4: D3D12（Windows CI ホスト）
                                     └─ v0.4: Metal 4（macOS CI ホスト）
```
境界の設計原則: 描画命令列は**値の列**（ツリー参照をbackendに持ち込まない）。
これがなければ Vulkan/D3D12/Metal の三又は抽象化詐欺になるところを、実装が 1 つでも検証できない時点で
インタフェースを凍結するのは早すぎる一般化——**まず 2 個目の実装（ソフトピクセル）を作る時に凍結する**。

## 6. ロードマップ（マイルストーンと検証基準）

| 版 | 内容 | 完了の客観基準 |
|---|---|---|
| v0.1 ✅ | 垂直スライス: パース〜端末描画。テスト・fuzz・ベンチ・ゴールデン | 本コミット |
| v0.2 ✅ | **適合性マイルストーン（2026-08-07 完了）**: WPT tree-construction 採点ハーネス（`tests/wpt-tree-construction/`、WPT master `5b6a1e6` ピン・61 ファイル 1,934 テスト byte-exact 一致を照合済）。**実行可能 1,922/1,922 = 100.0%**（skip は `#script-on` 12 件のみ = scripting UA 前提・恒久）。fragment 解析（WHATWG 13.4）実装で #document-fragment 196 件も全合格。WHATWG 全実体参照 2,125 名・quirks・foster・AAA・table モード群・in-template・foreign content 完全表まで搭載。HTTP/1.1 クライアント（src/net.c、plaintext・防御的 4 パーサ + fuzz_net）も搭載済。**文字コード層（src/charset.c、正本 docs/CHARSET.md）搭載済: Shift_JIS 系/EUC-JP → UTF-8 正規化、変換表は python codec 生成の再生成一致オラクル凍結、波ダッシュ 6 件は cp932 採用と明記**。以降: マージン相殺親子貫通、テーブルレイアウト | 100.0% 維持（後退は全件 oracle で即検出） |
| v-chrome ✅ | **GUI クローム（製品最終形）**: 生 X11 単一 UI（TUI は廃止、`--ui` は案内のみ）にタブ・オムニボックス・スクロール・リンクフォーカス/hover/クリック・ステータス帯を実装。slice-2 永続化（session/history/bookmarks を tmp→rename→fsync で単一 dir 原子管理・遅延復元・restore-first 起動）・自前フォント（5x7 + 16x16 全角、かな全量・漢字 103 字）・`--shot` 決定ラスタ検証（gui_smoke 51 checks）・md fast-DOM ロード・拡張 E1（akl、戻り値効果スキーマ、docs/EXTENSIONS.md）。残: グループ折りたたみ・プリセット・履歴/ブックマーク連携の候補・ダッシュボード、漢字第 6 陣、anchor 遷移 | 全ゲート緑 + GUI smoke 緑 |
| v0.3 | ソフトピクセルラスタ + backend 境界凍結（**製品の最終形は GUI（2026-07-29 ユーザ決定）**。TUI/ANSI 層はこの headless コンテナでの正確性・性能ゲート用途の検証バックエンドと位置づける。`exe を開いて Edge/Chrome と同じように使える` が最終形） + ~~画像デコード~~（**v0.4 で先行実装済み: src/image.c**。ImageMagick には頼らない）+ Vulkan（このコンテナで headless 検証可能なら） | 同一文書のセル版・ピクセル版で視覚一貫 |
| v0.4 | **Akl（自作 JS エンジン、ユーザ決定 2026-07-29）**: C11・JIT なし（JIT=攻撃面+メモリの両方を排除。W^X 全域、実行可能書き込みページゼロを構造保証。ユーザ提示動機「半分弱のウイルス無効化」は未検証値として扱い、検証可能な不変条件のみをここに採用する）。値は NaN-boxed 8B、ヒープ参照は obj 配列の u32 index。**v0.0 ✅（2026-07-29）: 字句 → recursive-descent → one-pass codegen → 全検証 verifier（opcode/即値/ジャンプ先境界/locals 参照/命令開始 bitmap）→ スタック VM**（計画の木歩きより強い形で着陸。dispatch は computed-goto/switch のデュアル構成で実測裁定 = goto 既定、1.005〜1.295×, BENCH.md。NaN/Inf 定数・ToString 往復最短精度・短絡生値・loose/strict 等価・全 budget fail-stop まで入る）。**v0.1: 配列/オブジェクト + mark-sweep GC**。DOM バインディング（querySelector・textContent・style 書換）は GC 安定後。**v0.2 ✅（2026-07-31）: JS 例外（throw/try/catch/finally、cross-frame 巻き戻し、v0.1 限界=try 越境 break/continue は明白な compile エラー）+ CoJIT（静的検証駆動の AOT 特化、runtime codegen ゼロで JIT 禁止に抵触しない。特化後は verify 再走査＋on/off 差分オラクルで機械監査）+ 例外機構の cold 分離（inline 展開で arith+24%/branch+42% のレイアウト悪化を同定・復元）**。**v0.3 ✅（2026-08-08、ユーザ指令により v0.4 計画から前倒し: 「akl が立ってないとブラウザとして正常に動作しない」）**: **ネイティブ登録層**（AKL_OK_NATIVE・`akl_native_register`/`akl_global_set`/`akl_mkobject`/`akl_prop_set`/`akl_tostring` 公開面、メソッド呼出 self 伝播（native のみ・`this` は言語に非導入）、1 呼出 1024 insn 固定課金 `AKL_NATIVE_COST`、`akl_native_throw` の明白失敗規約、nursery 一時ルート保護、登録系は VM 停止中限定で構造拒否、native 内からの再帰 eval 拒否）+ **オブジェクトモデル**（AKL_OK_OBJ: `{k:v}` リテラル・`.prop`・`.method()`・代入・参照共有・identity `===`。1 obj 64 prop 上限、prop name は intern STR で GC 伝播 mark。AklObj は 32→48B で mine は live 数比例のみ）＋拡張全 RT の `console.log` 常設（docs/EXTENSIONS.md §3-A・ext_smoke 12 checks）。配列・ブラケット `[i]`・関数式/クロージャは明白拒否のまま（AKL_COMPAT.md 実測表）。**DOM バインディング本体（querySelector 等）と `<script>` 実行配線は未実装（ここが次）** | JS からのレイアウト再計算が end-to-end で動く |
| v0.4b | **array-DOM（ユーザ提示「ツリーを Array に」を採択）**: IfNode をポインタの森から u32 index リンクの巨大配列へ。目的はメモリ理論極限（ノード当たりバイト数を半減見積——計測値を BENCH.md に出してから移行。未計測の見積 promise は書かない）| 移行前後で全ゲート無変化 + ノード当たりバイト数の公開 |
| v0.5 | プロセス分離（サイト分離）＋ seccomp サンドボックス。footprint/RSS への影響を数値で示してから適用 | 攻撃面評価を文書化、境界テスト追加 |
| 常時 | footprint ラチェット・fuzz 拡大・依存ゼロ維持 | BENCH.md 更新 |

## 7. 性能モデル（Amdahl の現在地）

現行支配項は **parse → layout**（2MB 文書で parse 7.2 / layout 7.9 / render 2.4 / style 0.0ms、16MB で parse 56.5 / layout 66.1 / render 15.3ms。実測値は BENCH.md を正とする）。
style は lazy 化で支配外。クリティカルパス外の最適化はしない。
平均ではなく大文書のワーストケースを見るのがこのプロジェクトの作法。

### 7.1 v0.3（2026-08 現行）支配構造とイベント直結パイプライン設計

16MB 線形 CLI の gprof 自己時間（20 run 累積、`-pg -fno-ipa-icf`・LTO 非適用のため
コール過多測で % は構造読み専用）: build_impl 11.4 / wrap_push_merge 8.5 / fitdom_text 7.5 /
layout_children 6.9 / layout_element 4.2 / wrap_text 4.2 / sweep_range 4.1 / scan_special_avx2 3.6 /
layout_ifc 3.2 / md_parse_fast_f 3.1 / run_flush 2.8 / arena_new_block 2.4（THP stall 課金）/
mo_open_push 2.4 / row_emit_direct 2.4 / geom_get 2.2 / blocks_win 2.2 / mo_close 2.0 / mo_text 2.0%。

現在の 3 段構造: **md→DOM(1.6M ノード書込) → layout が DOM を再読(2 pass: fitdom+wrap) →
render が lines を再読**。段ごとの強制トラフィック（書込 ~140MB + 全量再読 ×2-3）が
上限律速。イベント直結化 = md が構造イベント（open/close/text）を発行し、
layout がブロック完成ごとに消費する融合段にして DOM 素材化を局所に閉じ込める案。

確定している設計判断（着手前に真として固定）:
- **表・リスト・引用は先読みが要る**: ブロック全体の子を見て幅改行が決まる構造は
  「ブロック局所バッファ」まで縮退可能。文書全体の保持が必要なのはリンク目次と
  並列 layout の中間点だけ → 保持は (tag, attrs 最小, テキスト範囲) の幽霊骨格でよい。
- **byte-exact オラクルが命綱**: 融合しても最終発行ビットは現行と 1bit も変えない
  （chk_oracle 21/21: forged/2MB/16MB × out/ansi/w40/w160/dom/links/styles + serial≡sliced + script 実行 ON/OFF 双方向 + Shift_JIS/EUC-JP E2E 4 件 + 変換表再生成一致の全一致を逐次検証）。
- **2 pass → 1 pass 化は段階投入**: まず fitdom の融合（DOM 再読 1 回消去）、
  ついで DOM 局所化。見積もり便益は DOM トラフィック消去で -25~35ms（推定・上限）。
- 既判の不採用（再挑戦禁止）: POPULATE_WRITE 一括化・arena 16MB・-O3・style 2-way 並列。
  静的 micro 系列（AVX2 可視ラン等）は機構的優位のみを採用。

## 8. 開発規律

- ビルドは `-Wall -Wextra -Wshadow -Wstrict-prototypes -Wwrite-strings` 警告ゼロ維持。
- 常時ランタイム検証: `make test`（ASan+UBSan）、`make golden`（厳密 diff）、`make fuzz`（mutation + ASan）。
- この環境で検証不能なもの（GUI/GPU/他 OS）は「未検証」と明記して先送りする。**未検証コードを成果に数えない**。
