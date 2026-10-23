// PGO training: integer / bitwise code. AND, OR, XOR, SHL, SHR, MOD, DIV on int, uint and byte; hashing, gcd, primes, popcount.
set seed: uint = 2463534242;

fn xorshift() -> uint {
    seed = seed ^ (seed << 13);
    seed = seed ^ (seed >> 17);
    seed = seed ^ (seed << 5);
    return seed;
}

fn gcd(a: int, b: int) -> int {
    while (b != 0) {
        set t = a % b;
        a = b;
        b = t;
    }
    return a;
}

fn popcount(v: uint) -> int {
    set c = 0;
    while (v != 0) {
        c = c + int(v & 1);
        v >>= 1;
    }
    return c;
}

fn is_prime(n: int) -> bool {
    if (n < 2) { return false; }
    if (n % 2 == 0) { return n == 2; }
    for (set d = 3; d * d <= n; d = d + 2) {
        if (n % d == 0) { return false; }
    }
    return true;
}

fn fnv(n: int) -> uint {
    set h: uint = 2166136261;
    for (set i = 0; i < n; i = i + 1) {
        h = h ^ uint(i & 255);
        h = h * 16777619;
    }
    return h;
}

fn modpow(b: int, e: int, m: int) -> int {
    set r = 1;
    b = b % m;
    while (e > 0) {
        if (e % 2 == 1) { r = r * b % m; }
        b = b * b % m;
        e >>= 1;
    }
    return r;
}

fn digits_sum(n: int) -> int {
    set s = 0;
    while (n > 0) { s = s + n % 10; n = n / 10; }
    return s;
}

fn byte_mix(n: int) -> int {
    set a: byte = byte(1);
    set b: byte = byte(250);
    set acc = 0;
    for (set i = 0; i < n; i = i + 1) {
        a = a + byte(3);
        b = b - byte(7);
        a = a ^ b;
        b = b | byte(i % 3);
        acc = acc + int(a & byte(15)) + int(b);
    }
    return acc;
}

// int (arithmetic) and uint (logical) shifts by a register, by a constant and of a constant, and byte shifts
fn shift_mix(n: int) -> int {
    set acc = 0;
    set h: uint = 1;
    for (set i = 0; i < n; i++) {
        set k = i & 15;
        acc += (1 << k) >> 3;            // constant << register, then register >> constant
        acc -= (i - 30000) >> (k & 7);   // arithmetic shift of negative numbers
        h = (h << 5) ^ (h >> uint(k)) ^ uint(i);   // the count has the type of the value shifted
        acc += int((h >> 27) & 7);
        set b: byte = byte(i & 255);
        acc += int((b << 1) >> 2);
    }
    return acc;
}

fn main() {
    set x: uint = 0;
    for (set i = 0; i < 60000; i = i + 1) { x = x + xorshift() % 1000; }
    print(x); print("\n");

    set g = 0;
    for (set i = 1; i < 700; i = i + 1) {
        for (set j = 1; j < 120; j = j + 1) { g = g + gcd(i * 7, j * 11); }
    }
    print(g); print("\n");

    set pc = 0;
    for (set i = 0; i < 6000; i = i + 1) { pc = pc + popcount(uint(i) * 2654435761); }
    print(pc); print("\n");

    set np = 0;
    for (set i = 0; i < 40000; i = i + 1) { if (is_prime(i)) { np = np + 1; } }
    print(np); print("\n");

    print(fnv(200000)); print("\n");

    set mp = 0;
    for (set i = 1; i < 6000; i = i + 1) { mp = (mp + modpow(i, 65537, 30011)) % 30011; }
    print(mp); print("\n");

    set ds = 0;
    for (set i = 0; i < 80000; i = i + 1) { ds = ds + digits_sum(i); }
    print(ds); print("\n");

    print(byte_mix(120000)); print("\n");

    // signed arithmetic, negatives, comparison chains
    set neg = 0;
    for (set i = -3000; i < 3000; i = i + 1) {
        if (i < 0 && i % 3 == 0) { neg = neg - i / 3; }
        else if (i >= 0 && i % 5 != 0 || i == 0) { neg = neg + i; }
        if (!(i > 100) && i != -7) { neg = neg + 1; }
    }
    print(neg); print("\n");

    print(shift_mix(100000)); print("\n");
}
