// PGO training: call-heavy code. Recursion, many arguments (some passed on the stack), mutual recursion,
// functions with several live values, globals read and written inside hot calls.
set calls = 0;
set depth_max = 0;

fn fib(n: int) -> int {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}

fn ack(m: int, n: int) -> int {
    if (m == 0) { return n + 1; }
    if (n == 0) { return ack(m - 1, 1); }
    return ack(m - 1, ack(m, n - 1));
}

fn tak(x: int, y: int, z: int) -> int {
    if (y < x) {
        return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y));
    }
    return z;
}

fn is_even(n: int) -> bool {
    if (n == 0) { return true; }
    return is_odd(n - 1);
}

fn is_odd(n: int) -> bool {
    if (n == 0) { return false; }
    return is_even(n - 1);
}

// eight parameters: the last ones travel on the stack
fn mix8(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int {
    calls = calls + 1;
    return (a * 3 + b * 5 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + h * 23) % 1000003;
}

fn mix3(a: int, b: int, c: int) -> int {
    calls = calls + 1;
    return (a + b * 31 + c * 131) % 65521;
}

fn depth(n: int, d: int) -> int {
    if (d > depth_max) { depth_max = d; }
    if (n == 0) { return d; }
    return depth(n - 1, d + 1);
}

fn collatz_len(n: int) -> int {
    set steps = 0;
    while (n != 1) {
        if (n % 2 == 0) { n = n / 2; } else { n = 3 * n + 1; }
        steps = steps + 1;
    }
    return steps;
}

fn main() {
    print(fib(27)); print("\n");
    print(ack(2, 40)); print("\n");
    print(ack(3, 3)); print("\n");
    print(tak(18, 12, 6)); print("\n");

    set e = 0;
    for (set r = 0; r < 200; r = r + 1) {
        for (set i = 0; i < 50; i = i + 1) {
            if (is_even(i)) { e = e + 1; }
        }
    }
    print(e); print("\n");

    set acc = 0;
    for (set i = 0; i < 120000; i = i + 1) {
        acc = (acc + mix8(i, acc, i + 1, 2, 3, i % 7, 5, acc % 13)) % 1000003;
        acc = (acc + mix3(i, acc, 9)) % 1000003;
    }
    print(acc); print("\n");
    print(calls); print("\n");

    set tot = 0;
    for (set i = 0; i < 300; i = i + 1) { tot = tot + depth(30, 0); }
    print(tot); print(depth_max); print("\n");

    set best = 0;
    set besti = 0;
    for (set i = 1; i < 12000; i = i + 1) {
        set l = collatz_len(i);
        if (l > best) { best = l; besti = i; }
    }
    print(besti); print(best); print("\n");
}
