/* Akl — V8 API ファサード（C++ 形状互換, header-only）
 *
 * 目的: Akl（src/akl/akl.c, C11, JIT なし）を V8 API と同じ型形状で C++ から使う。
 * **互換範囲の唯一の定義は docs/V8_COMPAT.md の coverage map**。これは概念・型形状の
 * サブセット互換であり、V8 の ABI・完全挙動互換ではない。
 *
 * 設計の骨格:
 *   - Isolate ≈ AklRT（単一 realm。Context は Isolate への参照でしかない）
 *   - Local<T> は 8B cell を値で保持。T（Value/Number/Boolean/String/Context/Script）
 *     は空タグクラスで、メソッドは this を cell へのポインタとして読む
 *     （V8 が Local<T> を handle スロットへのポインタとして実装するのと同型。
 *       値の cell は AklVal、Context は Isolate*、Script は ScriptRec* を格納）
 *   - **HandleScope は RAII ノーオペ**: Akl の Local は GC ルートを張らない
 *     （Akl はヒープ参照を API に出さない設計の帰結）。未参照ヒープ値は次回 GC で
 *     回収され得る → Utf8Value へ写すか即時使用が利用者側責務
 *   - TryCatch: eval 失敗の捕捉。Akl は例外「値」を API に出さないため
 *     Exception() 相当は ExceptionString()（engine err 文字列、TryCatch 所有）
 *   - IsString だけは V8 形状からの明示偏差: 文字列判定は engine の obj 表を読む
 *     ため v->IsString(isolate) 形を取る
 *
 * 依存は <cstdint> <cstring> <cstdlib> <cstddef> のみ（libstdc++ シンボル不使用、
 * -fno-exceptions -fno-rtti で構築可 → 製品 ldd 不変条件を侵さない）。
 */
#ifndef IFUTO_V8_H
#define IFUTO_V8_H

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstddef>

/* <new>（libstdc++）を引かないための最小配置 new（非置換形の再宣言は合法） */
inline void *operator new(std::size_t, void *p) throw() { return p; }

#include "akl/akl.h"

namespace v8 {

class Isolate;
class Context;

namespace internal {
/* cell を this からビット正確に読む（エイリアシング規則に配慮して memcpy 経由） */
static inline uint64_t read_cell(const void *self) {
    uint64_t c; std::memcpy(&c, self, sizeof c); return c;
}
} /* namespace internal */

template <typename T>
class Local {
public:
    Local() : cell_(0), has_(false) {}
    bool IsEmpty() const { return !has_; }
    T *operator->() { return reinterpret_cast<T *>(&cell_); }
    const T *operator->() const { return reinterpret_cast<const T *>(&cell_); }
    template <typename S> Local<S> As() const { return Local<S>::from(cell_, has_); }
    static Local<T> from(uint64_t cell, bool has) { Local<T> l; l.cell_ = cell; l.has_ = has; return l; }
    uint64_t cell_bits() const { return cell_; }
private:
    uint64_t cell_;
    bool has_;
};

template <typename T>
class Maybe {
public:
    Maybe() : has_(false), v_() {}
    explicit Maybe(T v) : has_(true), v_(v) {}
    bool IsJust() const { return has_; }
    bool IsNothing() const { return !has_; }
    T FromMaybe(T default_value) const { return has_ ? v_ : default_value; }
private:
    bool has_; T v_;
};

class Value {
public:
    bool IsNumber() const { double d; return akl_as_num(cell(), &d); }
    bool IsString(Isolate *isolate) const;   /* 形状偏差（上部参照） */
    bool IsBoolean() const { bool b; return akl_as_bool(cell(), &b); }
    bool IsNull() const { return akl_is_null(cell()); }
    bool IsUndefined() const { return akl_is_undefined(cell()); }
    Maybe<double> NumberValue(const Local<Context> &context) const;
    Maybe<bool> BooleanValue(const Local<Context> &context) const;
protected:
    uint64_t cell() const { return internal::read_cell(this); }
};

class Number : public Value {
public:
    static Local<Number> New(Isolate *isolate, double value);
};

class Boolean : public Value {
public:
    static Local<Boolean> New(Isolate *isolate, bool value);
};

class String : public Value {
public:
    static Local<String> NewFromUtf8(Isolate *isolate, const char *data, int length = -1);
    class Utf8Value {
    public:
        Utf8Value(Isolate *isolate, const Local<Value> &v);
        ~Utf8Value() { std::free(str_); }
        const char *data() const { return str_ ? str_ : ""; }
        int length() const { return len_; }
        const char *operator*() const { return data(); }
    private:
        char *str_; int len_;
        Utf8Value(const Utf8Value &);            /* non-copyable（V8 と同じ規律） */
        Utf8Value &operator=(const Utf8Value &);
    };
};

Local<Value> Undefined(Isolate *isolate);
Local<Value> Null(Isolate *isolate);

class Context {
public:
    static Local<Context> New(Isolate *isolate);
    Isolate *GetIsolate() const;
    class Scope {
    public:
        explicit Scope(const Local<Context> &) {}  /* 単一 realm: 形状のみ */
    private:
        Scope(const Scope &);
        Scope &operator=(const Scope &);
    };
};

class Script {
public:
    static Local<Script> Compile(const Local<Context> &context, const Local<String> &source);
    /* V8 偏差: Akl は parse+run 一体のため構文エラーは Compile ではなく Run で
     * 顕在化する。失敗すると empty を返し、稼働中の TryCatch が捕捉する */
    Local<Value> Run(const Local<Context> &context);
};

class TryCatch {
public:
    explicit TryCatch(Isolate *isolate);
    ~TryCatch();
    bool HasCaught() const { return err_ != NULL; }
    const char *ExceptionString() const { return err_ ? err_ : ""; }
    void Reset() { std::free(err_); err_ = NULL; }
    /* 内部: engine err の取り込み（Script::Run / Isolate::Eval 専用） */
    void steal(const char *msg);
private:
    Isolate *iso_;
    TryCatch *prev_;
    char *err_;
    TryCatch(const TryCatch &);
    TryCatch &operator=(const TryCatch &);
};

class Isolate {
public:
    static Isolate *New();
    void Dispose();
    class HandleScope {
    public:
        explicit HandleScope(Isolate *) {}         /* RAII ノーオペ（設計: 上部参照） */
    private:
        HandleScope(const HandleScope &);
        HandleScope &operator=(const HandleScope &);
    };
    class Scope {
    public:
        explicit Scope(Isolate *) {}
    };
    /* eval 便宜 API（Script 形状を取らない最短経路。失敗 false＝稼働中の TryCatch が捕捉） */
    bool Eval(const char *source, Local<Value> *out);
private:
    struct ScriptRec { ScriptRec *next; char *src; };
    struct Impl {
        AklRT *rt;
        TryCatch *tc;
        ScriptRec *scripts;
        Impl() : rt(NULL), tc(NULL), scripts(NULL) {}
    };
    Impl *impl_;
    Isolate() : impl_(NULL) {}
    ~Isolate() {}
    Isolate(const Isolate &);
    Isolate &operator=(const Isolate &);
    /* cell 読解系の friend（obj 表アクセスが Isolate::impl_ 経由のため） */
    friend class Value;
    friend class String;
    friend class String::Utf8Value;
    friend class Context;
    friend class Script;
    friend class TryCatch;
};

/* ============================ 実装 ============================ */

inline Isolate *Isolate::New() {
    void *mem = std::malloc(sizeof(Isolate));
    if (!mem) return NULL;
    Isolate *iso = new (mem) Isolate();
    iso->impl_ = new (std::malloc(sizeof(Impl))) Impl();
    if (!iso->impl_) { std::free(iso); return NULL; }
    iso->impl_->rt = akl_new();
    if (!iso->impl_->rt) { std::free(iso->impl_); std::free(iso); return NULL; }
    return iso;
}

inline void Isolate::Dispose() {
    if (!impl_) { std::free(this); return; }
    if (impl_->rt) akl_free(impl_->rt);
    while (impl_->scripts) {
        ScriptRec *n = impl_->scripts->next;
        std::free(impl_->scripts->src);
        std::free(impl_->scripts);
        impl_->scripts = n;
    }
    std::free(impl_);
    impl_ = NULL;
    std::free(this);
}

inline bool Isolate::Eval(const char *source, Local<Value> *out) {
    uint64_t v = 0;
    bool ok = akl_eval(impl_->rt, source, &v);
    if (!ok && impl_->tc) impl_->tc->steal(akl_error(impl_->rt));
    if (out) *out = ok ? Local<Value>::from(v, true) : Local<Value>();
    return ok;
}

inline Local<Context> Context::New(Isolate *isolate) {
    return Local<Context>::from(reinterpret_cast<uint64_t>(isolate), isolate != NULL);
}
inline Isolate *Context::GetIsolate() const {
    return reinterpret_cast<Isolate *>(internal::read_cell(this));
}

inline bool Value::IsString(Isolate *isolate) const {
    return isolate && akl_is_string(isolate->impl_->rt, cell());
}
inline Maybe<double> Value::NumberValue(const Local<Context> &) const {
    double d;
    return akl_as_num(cell(), &d) ? Maybe<double>(d) : Maybe<double>();
}
inline Maybe<bool> Value::BooleanValue(const Local<Context> &) const {
    bool b;
    return akl_as_bool(cell(), &b) ? Maybe<bool>(b) : Maybe<bool>();
}

inline Local<Number> Number::New(Isolate *, double value) {
    return Local<Number>::from(akl_mknum(value), true);
}
inline Local<Boolean> Boolean::New(Isolate *, bool value) {
    return Local<Boolean>::from(akl_mkbool(value), true);
}
inline Local<Value> Undefined(Isolate *) { return Local<Value>::from(akl_mkundefined(), true); }
inline Local<Value> Null(Isolate *) { return Local<Value>::from(akl_mknull(), true); }

inline Local<String> String::NewFromUtf8(Isolate *isolate, const char *data, int length) {
    if (!isolate || !data) return Local<String>();
    uint32_t n = length >= 0 ? (uint32_t)length : (uint32_t)std::strlen(data);
    uint64_t v = akl_mkstring(isolate->impl_->rt, data, n);
    if (akl_is_undefined(v)) return Local<String>();  /* err 設定済（budget/oom） */
    return Local<String>::from(v, true);
}
inline String::Utf8Value::Utf8Value(Isolate *isolate, const Local<Value> &v) : str_(NULL), len_(0) {
    if (!isolate || v.IsEmpty()) return;
    uint32_t n = 0;
    const char *s = akl_as_str(isolate->impl_->rt, v.cell_bits(), &n);
    if (!s) return;
    str_ = (char *)std::malloc((size_t)n + 1);
    if (!str_) return;
    std::memcpy(str_, s, n); str_[n] = '\0'; len_ = (int)n;
}

inline void TryCatch::steal(const char *msg) {
    std::free(err_);
    err_ = NULL;
    if (!msg || !*msg) return;
    size_t n = std::strlen(msg) + 1;
    err_ = (char *)std::malloc(n);
    if (err_) std::memcpy(err_, msg, n);
}
inline TryCatch::TryCatch(Isolate *isolate) : iso_(isolate), prev_(NULL), err_(NULL) {
    if (iso_ && iso_->impl_) { prev_ = iso_->impl_->tc; iso_->impl_->tc = this; }
}
inline TryCatch::~TryCatch() {
    if (iso_ && iso_->impl_ && iso_->impl_->tc == this) iso_->impl_->tc = prev_;
    std::free(err_);
}

inline Local<Script> Script::Compile(const Local<Context> &context, const Local<String> &source) {
    if (context.IsEmpty() || source.IsEmpty()) return Local<Script>();
    Isolate *iso = context->GetIsolate();
    if (!iso || !iso->impl_) return Local<Script>();
    String::Utf8Value utf(iso, source.As<Value>());
    Isolate::ScriptRec *rec = (Isolate::ScriptRec *)std::malloc(sizeof(Isolate::ScriptRec));
    if (!rec) return Local<Script>();
    size_t n = std::strlen(*utf) + 1;
    rec->src = (char *)std::malloc(n);
    if (!rec->src) { std::free(rec); return Local<Script>(); }
    std::memcpy(rec->src, *utf, n);
    rec->next = iso->impl_->scripts;
    iso->impl_->scripts = rec;
    return Local<Script>::from(reinterpret_cast<uint64_t>(rec), true);
}

inline Local<Value> Script::Run(const Local<Context> &context) {
    Isolate::ScriptRec *rec = reinterpret_cast<Isolate::ScriptRec *>(internal::read_cell(this));
    if (!rec || context.IsEmpty()) return Local<Value>();
    Isolate *iso = context->GetIsolate();
    uint64_t v = 0;
    if (!akl_eval(iso->impl_->rt, rec->src, &v)) {
        if (iso->impl_->tc) iso->impl_->tc->steal(akl_error(iso->impl_->rt));
        return Local<Value>();
    }
    return Local<Value>::from(v, true);
}

} /* namespace v8 */

#endif
