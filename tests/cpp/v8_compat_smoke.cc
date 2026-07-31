/* V8 API ファサード（src/v8x/v8.h）の実動スモーク。
 * 2 つの証明: (1) V8 形状 API が実際に計算を駆動する、
 *            (2) このテストバイナリが libstdc++ を動的リンクしない（Makefile が検査）。
 * 「V8 互換」の範囲は docs/V8_COMPAT.md が唯一の定義。 */
#include <cstdio>
#include <cstring>
#include "../../src/v8x/v8.h"

static int g_checks = 0, g_fails = 0;
#define CK(cond) do { g_checks++; if (!(cond)) { g_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main() {
    using namespace v8;

    /* Isolate ≈ V8xRT */
    Isolate *iso = Isolate::New();
    CK(iso != NULL);
    {
        Isolate::HandleScope hs(iso);           /* RAII ノーオペ（形状のみ） */
        Local<Context> ctx = Context::New(iso);
        Context::Scope cs(ctx);
        CK(!ctx.IsEmpty());

        /* 値生成・検査（V8 形状） */
        Local<Number> n = Number::New(iso, 1.5);
        Local<Value> nv = n.As<Value>();
        CK(!nv.IsEmpty());
        CK(nv->IsNumber());
        CK(!nv->IsString(iso));
        CK(!nv->IsNull() && !nv->IsUndefined() && !nv->IsBoolean());
        CK(nv->NumberValue(ctx).FromMaybe(0.0) == 1.5);

        Local<Boolean> b = Boolean::New(iso, true);
        CK(b.As<Value>()->IsBoolean());
        CK(b.As<Value>()->BooleanValue(ctx).FromMaybe(false) == true);

        Local<Value> un = Undefined(iso), nu = Null(iso);
        CK(un->IsUndefined() && !un->IsNull());
        CK(nu->IsNull() && !nu->IsUndefined());

        /* 文字列往復（engine GC ヒープ → ホスト写し） */
        Local<String> s = String::NewFromUtf8(iso, "hello-aklus");
        CK(!s.IsEmpty());
        CK(s.As<Value>()->IsString(iso));
        CK(!s.As<Value>()->IsNumber());
        String::Utf8Value u8(iso, s.As<Value>());
        CK(u8.length() == 11);
        CK(std::strcmp(*u8, "hello-aklus") == 0);

        /* Script::Compile / Run（V8 形状。eval 駆動） */
        Local<String> src = String::NewFromUtf8(iso, "var q=21; q*2");
        Local<Script> sc = Script::Compile(ctx, src);
        CK(!sc.IsEmpty());
        Local<Value> r = sc->Run(ctx);
        CK(!r.IsEmpty());
        CK(r->IsNumber());
        CK(r->NumberValue(ctx).FromMaybe(0.0) == 42.0);

        /* realm 状態は Isolate に保持（文脈を跨いで var が残る = V8 同じ感覚） */
        Local<String> src2 = String::NewFromUtf8(iso, "q+1");
        Local<Script> sc2 = Script::Compile(ctx, src2);
        CK(sc2->Run(ctx)->NumberValue(ctx).FromMaybe(0.0) == 22.0);

        /* TryCatch: 失敗の捕捉（例外値ではなく err 文字列 = 明示偏差） */
        {
            TryCatch tc(iso);
            CK(!tc.HasCaught());
            Local<String> bad = String::NewFromUtf8(iso, "throw 'boom'");
            Local<Script> sb = Script::Compile(ctx, bad);
            Local<Value> br = sb->Run(ctx);
            CK(br.IsEmpty());
            CK(tc.HasCaught());
            CK(std::strstr(tc.ExceptionString(), "uncaught exception") != NULL);
            CK(std::strstr(tc.ExceptionString(), "boom") != NULL);
            tc.Reset();
            CK(!tc.HasCaught());
        }
        /* TryCatch 非稼働でも eval 自体は安全に失敗を返す */
        Local<String> bad2 = String::NewFromUtf8(iso, "1 +* 2");
        Local<Script> sb2 = Script::Compile(ctx, bad2);
        CK(sb2->Run(ctx).IsEmpty());

        /* 例外で巻き戻っても次の eval は健全（エンジン側の独立性もここで刻む） */
        Local<String> ok2 = String::NewFromUtf8(iso, "q*3");
        CK(Script::Compile(ctx, ok2)->Run(ctx)->NumberValue(ctx).FromMaybe(0.0) == 63.0);
    }
    iso->Dispose();

    /* Isolate 独立性: 片方の var は他方から見えない */
    Isolate *a = Isolate::New(), *b2 = Isolate::New();
    CK(a && b2 && a != b2);
    {
        Local<Context> ca = Context::New(a);
        Local<Value> out;
        CK(a->Eval("var only=7; only", &out));
        CK(out->IsNumber());
        Local<Context> cb = Context::New(b2);
        (void)cb;
        Local<Value> outb;
        CK(!b2->Eval("only + 1", &outb)); /* ReferenceError → false（独立の証明） */
    }
    a->Dispose();
    b2->Dispose();

    fprintf(stderr, "v8 compat smoke: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
