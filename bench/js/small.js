// startup+parse dominated: many small functions, little execution.
// This is the "tab just opened, page scripts start" axis.
function f000(x){return x*2+1;}function f001(x){return x*3+2;}function f002(x){return x*5+3;}
function f003(x){return x*7+4;}function f004(x){return x*11+5;}function f005(x){return x*13+6;}
function f006(x){return x*17+7;}function f007(x){return x*19+8;}function f008(x){return x*23+9;}
function f009(x){return x*29+10;}function f010(x){return x*31+11;}function f011(x){return x*37+12;}
function f012(x){return x*41+13;}function f013(x){return x*43+14;}function f014(x){return x*47+15;}
function f015(x){return x*53+16;}function f016(x){return x*59+17;}function f017(x){return x*61+18;}
function f018(x){return x*67+19;}function f019(x){return x*71+20;}function f020(x){return x*73+21;}
var acc = 0;
for (var i = 0; i < 2000; i = i + 1) {
    acc = acc + f000(i) + f001(i) + f002(i) + f003(i) + f004(i);
    acc = (acc + f005(i) + f006(i) + f007(i) + f008(i) + f009(i)) % 100000;
    acc = (acc + f010(i) + f011(i) + f012(i) + f013(i) + f014(i)) % 100000;
    acc = (acc + f015(i) + f016(i) + f017(i) + f018(i) + f019(i) + f020(i)) % 100000;
}
var __R = acc;
__R;
