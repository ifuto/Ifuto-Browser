// non-recursive call throughput with local-heavy bodies.
function add3(a, b, c) { var t = a + b; var u = t + c; return u; }
var acc = 0;
for (var i = 0; i < 300000; i = i + 1) { acc = add3(acc, i, 1) % 100000; }
var __R = acc;
__R;
