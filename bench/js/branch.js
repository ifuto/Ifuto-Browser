var acc = 0; for (var i = 0; i < 2000000; i = i+1) { var t = i * 2; if (t % 5 == 0) { acc = acc + t; } else { acc = acc + 1; } } acc
