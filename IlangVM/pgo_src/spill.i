// PGO training: register pressure. Long expressions that need more temporaries than there are registers (the
// compiler spills them to frame slots), functions with more local variables than registers (LOADLOCAL / STORELOCAL),
// and calls in the middle of expressions (caller-saved registers pushed and popped).
fn poly(x: float) -> float {
    return ((((((x * 0.5 + 1.25) * x - 2.5) * x + 3.75) * x - 1.125) * x + 0.0625) * x - 7.0) * (x * x + 1.0)
         + (x - 1.0) * (x + 2.0) * (x - 3.0) * (x + 4.0) / (x * x + 5.0);
}

fn many_locals(n: int) -> int {
    set a = n; set b = n + 1; set c = n + 2; set d = n + 3; set e = n + 4; set f = n + 5; set g = n + 6;
    set h = n + 7; set i = n + 8; set j = n + 9; set k = n + 10; set l = n + 11; set m = n + 12; set o = n + 13;
    set p = n + 14; set q = n + 15; set r = n + 16; set s = n + 17; set t = n + 18; set u = n + 19; set v = n + 20;
    for (set it = 0; it < 20; it++) {
        a += b; b ^= c; c += d * 3; d -= e; e += f; f ^= g; g += h; h -= i; i += j; j ^= k; k += l;
        l -= m; m += o; o ^= p; p += q; q -= r; r += s; s ^= t; t += u; u -= v; v += a;
    }
    return a + b + c + d + e + f + g + h + i + j + k + l + m + o + p + q + r + s + t + u + v;
}

fn sq(x: int) -> int { return x * x; }

fn nested_calls(n: int) -> int {
    set s = 0;
    for (set i = 0; i < n; i++) {
        s += sq(i % 100) + (sq(i % 7) * sq(i % 11) - sq(i % 13)) * (sq(i % 3) + sq(i % 5)) % 1000;
    }
    return s;
}

fn matrix_expr(a: float[][], b: float[][], n: int) -> float {
    set s = 0.0;
    for (set i = 1; i < n - 1; i++) {
        for (set j = 1; j < n - 1; j++) {
            s += (a[i][j] * b[j][i] + a[i - 1][j] * b[j][i - 1] - a[i + 1][j] * b[j][i + 1]) * (a[i][j - 1] + b[i][j + 1])
               - (a[i - 1][j - 1] - b[i + 1][j + 1]) * (a[i + 1][j - 1] + b[i - 1][j + 1]) / 8.0;
        }
    }
    return s;
}

fn main() {
    set acc = 0.0;
    for (set i = 0; i < 70000; i++) { acc += poly(float(i % 200) / 100.0 - 1.0); }
    print(acc); print("\n");

    set t = 0;
    for (set i = 0; i < 7000; i++) { t += many_locals(i); }
    print(t); print("\n");

    print(nested_calls(35000)); print("\n");

    set n = 90;
    set a: float[][] = [[0.0; n]; n];
    set b: float[][] = [[0.0; n]; n];
    for (set i = 0; i < n; i++) { for (set j = 0; j < n; j++) { a[i][j] = float((i * 7 + j * 3) % 17) / 4.0; b[i][j] = float((i + 2 * j) % 13) / 3.0; } }
    set s = 0.0;
    for (set r = 0; r < 7; r++) { s += matrix_expr(a, b, n); }
    print(s); print("\n");
}
