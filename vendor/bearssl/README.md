# BearSSL (vendored)

- 出典: https://www.bearssl.org / https://github.com/unkaktus/bearssl (mirror)
- バージョン: 0.6 相当（master ピン。TLS 1.2 まで）
- ライセンス: MIT（同梱 LICENSE 参照。Copyright (c) 2016 Thomas Pornin）
- 採用根拠: ARCHITECTURE.md §6 ロードマップ「TLS は BearSSL 等の battle-tested 物。
  自作 TLS は禁止」。静的リンクにより製品法則「ldd = vdso/libm/libc/ld」は維持。
- 統合: Makefile の BEARSRC が全 .c を LTO 一括コンパイルし、--gc-sections で
  使用シンボルのみ残す。include は vendor/bearssl/inc を -I に追加。
- 変更: 一切加えない（vendor は編集禁止。必要ならパッチは別途管理）。
