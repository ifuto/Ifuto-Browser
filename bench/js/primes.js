// trial-division primes: integer modulo + break in nested control flow.
var c = 0;
for (var n = 2; n < 12000; n = n + 1) {
    var p = 1;
    var d = 2;
    while (d * d <= n) { if (n % d == 0) { p = 0; break; } d = d + 1; }
    if (p) { c = c + 1; }
}
var __R = c;
__R;
