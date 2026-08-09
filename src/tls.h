/* Ifuto — TLS クライアント公開面（実装: src/tls.c、BearSSL 静的リンク）。 */
#ifndef IF_TLS_H
#define IF_TLS_H

#include "common.h"
#include <stddef.h>
#include <sys/types.h>

typedef struct IfTls IfTls;

/* fd は接続済み（ブロッキング・SO_*TIMEO 設定済み）前提。TLS 1.2 ハンドシェイク +
 * CA チェーン検証 + サーバ名照合を実行。成功で IfTls を返す。
 * 失敗時は NULL で *err に "tls" / "cert" / "ca" のいずれか。 */
IfTls *if_tls_client(int fd, const char *host, const char **err);

/* アプリデータ送信（内部でフラッシュまで行う）。false で *err 設定 */
bool if_tls_send_all(IfTls *t, const u8 *p, u64 n, const char **err);

/* アプリデータ受信。戻り値: >0 バイト数 / 0 = close_notify(EOF) / -1 = エラー */
ssize_t if_tls_recv(IfTls *t, u8 *p, u64 cap, const char **err);

/* close_notify 送信（ベストエフォート）して解放 */
void if_tls_close(IfTls *t);

#endif
