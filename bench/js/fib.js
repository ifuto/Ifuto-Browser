// call/branch/arith bound. V8x v0.0 subset: no ++, +=, ?:, arrays, objects, bitwise.
function fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
var __R = fib(26);
__R;
