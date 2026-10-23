// PGO training: byte[] and bool[] arrays (1-byte elements), MEMSET fills, unsafe blocks, sieve-style flag arrays, run-length
// coding and a checksum over a byte buffer.
fn sieve_bool(n: int) -> int {
    set flags: bool[] = [true; 100000];
    flags[0] = false; flags[1] = false;
    for (set i = 2; i * i < n; i = i + 1) {
        if (flags[i]) {
            for (set j = i * i; j < n; j = j + i) { flags[j] = false; }
        }
    }
    set c = 0;
    for (set i = 0; i < n; i = i + 1) { if (flags[i]) { c = c + 1; } }
    return c;
}

fn sieve_unsafe(n: int) -> int {
    set flags: bool[] = [true; 100000];
    unsafe {
        for (set i = 2; i * i < n; i = i + 1) {
            if (flags[i]) {
                for (set j = i * i; j < n; j = j + i) { flags[j] = false; }
            }
        }
    }
    set c = 0;
    unsafe { for (set i = 2; i < n; i = i + 1) { if (flags[i]) { c = c + 1; } } }
    return c;
}

fn fill_bytes(buf: byte[], n: int, seed: int) {
    set x = seed;
    for (set i = 0; i < n; i = i + 1) {
        x = (x * 75 + 74) % 65537;
        buf[i] = byte(x % 7 + 65);
    }
}

fn rle_len(buf: byte[], n: int) -> int {
    set runs = 1;
    for (set i = 1; i < n; i = i + 1) {
        if (buf[i] != buf[i - 1]) { runs = runs + 1; }
    }
    return runs;
}

fn adler(buf: byte[], n: int) -> int {
    set a = 1;
    set b = 0;
    for (set i = 0; i < n; i = i + 1) {
        a = (a + int(buf[i])) % 65521;
        b = (b + a) % 65521;
    }
    return b * 65536 + a;
}

fn histogram(buf: byte[], n: int) -> int {
    set h: int[] = [0; 256];
    for (set i = 0; i < n; i = i + 1) {
        set k = int(buf[i]);
        h[k] = h[k] + 1;
    }
    set best = 0;
    for (set i = 0; i < 256; i = i + 1) { if (h[i] > best) { best = h[i]; } }
    return best;
}

fn game_of_bytes(n: int, gens: int) -> int {
    // 1-D cellular automaton (rule 90 / 30 style) on a byte buffer
    set cur: byte[] = [0; 600];
    set nxt: byte[] = [0; 600];
    cur[n / 2] = byte(1);
    set alive = 0;
    for (set g = 0; g < gens; g = g + 1) {
        for (set i = 1; i < n - 1; i = i + 1) {
            set l = cur[i - 1];
            set c = cur[i];
            set r = cur[i + 1];
            nxt[i] = l ^ (c | r);
        }
        for (set i = 0; i < n; i = i + 1) { cur[i] = nxt[i]; alive = alive + int(cur[i]); }
    }
    return alive;
}

fn main() {
    set s = 0;
    for (set r = 0; r < 2; r = r + 1) {
        s = s + sieve_bool(100000);
        s = s + sieve_unsafe(100000);
    }
    print(s); print("\n");

    set buf: byte[] = [0; 20000];
    fill_bytes(buf, 20000, 17);
    print(rle_len(buf, 20000)); print("\n");
    set cs = 0;
    for (set r = 0; r < 4; r = r + 1) { cs = cs + adler(buf, 20000) % 1000; }
    print(cs); print("\n");
    set hs = 0;
    for (set r = 0; r < 4; r = r + 1) { hs = hs + histogram(buf, 20000); }
    print(hs); print("\n");
    print(game_of_bytes(500, 150)); print("\n");

    // many short-lived zero-filled and value-filled buffers
    set t = 0;
    for (set i = 0; i < 3000; i = i + 1) {
        set z: byte[] = [byte(i % 200); 64];
        set zz: int[] = [i; 32];
        t = t + int(z[i % 64]) + zz[i % 32] % 10;
    }
    print(t); print("\n");
}
