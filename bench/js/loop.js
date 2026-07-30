// global-var heavy arithmetic + integer modulo. Taxes variable access + ALU ops.
var s = 0;
var M = 100000;
for (var i = 0; i < 100000; i = i + 1) { s = (s + i * 3 + 1) % M; }
var __R = s;
__R;
