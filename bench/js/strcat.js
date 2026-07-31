// string concatenation growth (allocator + intern stress).
var s = '';
for (var i = 0; i < 100000; i = i + 1) { s = s + 'x'; }
var __R = s;
__R;
