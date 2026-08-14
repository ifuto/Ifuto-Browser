/* AKL 用 testharness.js サブセット（WPT テスト実行用。WPT 本体の resources/testharness.js
 * とは独立の軽量実装）。対応 API: test / async_test / promise_test / setup / done /
 * assert_equals / assert_true / assert_false / assert_throws_dom / assert_throws_js /
 * assert_array_equals / assert_unreached / generate_tests / format_value。
 *
 * 設計（AKL の同期エンジンに合わせた近似）:
 *  - async_test は「マイクロタスク drain が終わるまでに done されなければ timeout FAIL」。
 *  - promise_test は返り値 Promise の then で結果を記録（drain で実行される）。
 *  - 結果は __dump() が print で 1 行ずつ出力（ランナーが stdout を読む）。
 *  - fetch 等の未対応 API は ReferenceError としてそのテストが明白に FAIL する
 *    （黙ってスキップしない）。
 */
var __results = [];
var __async_pending = [];
var __single = false;
var __single_done = false;

function __fail(name, msg) {
    __results.push({ n: name, s: 0, m: msg || "" });
}
function __pass(name) {
    __results.push({ n: name, s: 1, m: "" });
}

function format_value(v) {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    if (typeof v === "string") return '"' + v + '"';
    if (typeof v === "number") return String(v);
    if (typeof v === "boolean") return String(v);
    if (typeof v === "function") return "<function>";
    try { return String(v); } catch (e) { return "<unstringable>"; }
}

function test(fn, name) {
    if (__single && __single_done) return; /* single_test 完了後は実行しない */
    try {
        fn();
        __pass(name || "(unnamed test)");
    } catch (e) {
        var m = (e && e.message) ? e.message : String(e);
        __fail(name || "(unnamed test)", m);
    }
}

function async_test(fn, name) {
    var t = {};
    t._done = false;
    t._name = name || "(unnamed async test)";
    t.step = function (fn) {
        try { fn.call(t); } catch (e) {
            var m = (e && e.message) ? e.message : String(e);
            __fail(t._name, m);
            t._done = true;
        }
    };
    t.step_func = function (fn) {
        var self = t;
        return function () {
            try { return fn.apply(this, arguments); } catch (e) {
                var m = (e && e.message) ? e.message : String(e);
                __fail(self._name, m);
                self._done = true;
            }
        };
    };
    t.step_func_done = function (fn) {
        var self = t;
        return function () {
            try { fn.apply(this, arguments); self._done = true; }
            catch (e) {
                var m = (e && e.message) ? e.message : String(e);
                __fail(self._name, m);
                self._done = true;
            }
        };
    };
    t.step_timeout = function (fn, ms) {
        var self = t;
        setTimeout(function () {
            try { fn.call(self); } catch (e) {
                var m = (e && e.message) ? e.message : String(e);
                __fail(self._name, m);
                self._done = true;
            }
        }, ms);
    };
    t.done = function () { t._done = true; };
    __async_pending.push(t);
    if (typeof fn === "function") {
        /* WPT の async_test(fn, name): fn を即時実行（t を引数に）。
         * メソッド（step_func_done 等）の設定後に呼ぶこと（順序誤りで
         * "not a function" になる実バグを修正済み） */
        try {
            fn(t);
        } catch (e) {
            var m = (e && e.message) ? e.message : String(e);
            __fail(t._name, m);
            t._done = true;
        }
    }
    return t;
}

function promise_test(fn, name) {
    var nm = name || "(unnamed promise test)";
    try {
        var p = fn();
        if (p && typeof p.then === "function") {
            p.then(function () { __pass(nm); },
                   function (e) {
                       var m = (e && e.message) ? e.message : String(e);
                       __fail(nm, m);
                   });
        } else {
            __fail(nm, "promise_test did not return a promise");
        }
    } catch (e) {
        var m = (e && e.message) ? e.message : String(e);
        __fail(nm, m);
    }
}

function setup(o) {
    if (o && o.single_test) __single = true;
}
function done() {
    __single_done = true;
}

function assert_equals(a, b, desc) {
    if (a !== b) {
        var m = (desc ? desc + ": " : "") + "expected " + format_value(b) +
                " got " + format_value(a);
        throw new Error(m);
    }
}
function assert_true(v, desc) {
    if (v !== true) {
        var m = (desc ? desc + ": " : "") + "expected true got " + format_value(v);
        throw new Error(m);
    }
}
function assert_false(v, desc) {
    if (v !== false) {
        var m = (desc ? desc + ": " : "") + "expected false got " + format_value(v);
        throw new Error(m);
    }
}
function assert_unreached(desc) {
    throw new Error((desc ? desc + ": " : "") + "unreached");
}
function assert_array_equals(a, b, desc) {
    if (!a || !b || a.length !== b.length) {
        throw new Error((desc ? desc + ": " : "") + "length mismatch: " +
                        format_value(a) + " vs " + format_value(b));
    }
    for (var i = 0; i < a.length; i++) {
        if (!(a[i] === b[i])) {
            throw new Error((desc ? desc + ": " : "") + "element " + i + ": " +
                            format_value(a[i]) + " vs " + format_value(b[i]));
        }
    }
}
function __err_name(e) {
    if (e && typeof e === "object" && e.name) return e.name;
    return String(e);
}
function assert_throws_dom(code, fn, desc) {
    var threw = false;
    try { fn(); } catch (e) {
        threw = true;
        if (__err_name(e) === code) return;
        throw new Error((desc ? desc + ": " : "") + "expected " + code +
                        " got " + __err_name(e));
    }
    if (!threw) throw new Error((desc ? desc + ": " : "") + "expected " + code +
                                " but nothing was thrown");
}
function assert_throws_js(errType, fn, desc) {
    var code = (errType && errType.name) ? errType.name : String(errType);
    var threw = false;
    try { fn(); } catch (e) {
        threw = true;
        if (__err_name(e) === code) return;
        throw new Error((desc ? desc + ": " : "") + "expected " + code +
                        " got " + __err_name(e));
    }
    if (!threw) throw new Error((desc ? desc + ": " : "") + "expected " + code +
                                " but nothing was thrown");
}

function generate_tests(generator, tests) {
    for (var i = 0; i < tests.length; i++) {
        var t = tests[i];
        var name = t[0];
        var args = t.slice(1);
        test(function () { generator.apply(null, args); }, name);
    }
}

/* マイクロタスク drain 後に呼ばれる（ランナーが queueMicrotask(__dump) を末尾に積む）:
 * 未完了 async_test を timeout FAIL にして、全結果を 1 行ずつ print する。 */
function __dump() {
    for (var i = 0; i < __async_pending.length; i++) {
        var t = __async_pending[i];
        if (!t._done) __fail(t._name, "timeout (async_test not done before microtask drain)");
    }
    for (var j = 0; j < __results.length; j++) {
        var r = __results[j];
        print((r.s === 1 ? "PASS " : "FAIL ") + r.n + (r.m ? " :: " + r.m : ""));
    }
}
