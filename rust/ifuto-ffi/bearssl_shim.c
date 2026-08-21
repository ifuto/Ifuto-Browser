/* BearSSL の `static inline` API（非エクスポート・構造体フィールド直アクセス）を
 * Rust から呼べる非インライン関数として提供する薄いシム。
 *
 * `br_ssl_engine_set_versions` / `br_ssl_engine_last_error` は `bearssl_ssl.h` の
 * static inline であり、共有シンボルが存在しない。Rust 側は `br_ssl_client_context` の
 * レイアウトを再現しない（不透明バッファ + `sizeof`）ため、ここで薄く包む。 */
#include "bearssl.h"

void ifuto_br_ssl_engine_set_versions(void *cc, unsigned version_min, unsigned version_max) {
    br_ssl_engine_set_versions((br_ssl_engine_context *)cc, version_min, version_max);
}

int ifuto_br_ssl_engine_last_error(const void *cc) {
    return br_ssl_engine_last_error((const br_ssl_engine_context *)cc);
}
