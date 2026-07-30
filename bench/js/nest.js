// nested-loop arithmetic (function-free hot inner body).
var a = 0;
var P = 100000;
for (var i = 0; i < 500; i = i + 1) {
    for (var j = 0; j < 800; j = j + 1) { a = (a + i * j) % P; }
}
var __R = a;
__R;
