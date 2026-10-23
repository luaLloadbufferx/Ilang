// PGO training: shifts and bit tricks. SHL / SHR on int (arithmetic), uint and byte (logical), by a register, by a
// constant and of a constant (1 << n); bitsets, a xorshift generator, bit reversal, popcount, CRC-32.
set state: uint = 2463534242;

fn xorshift32() -> uint {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

fn popcount(x: uint) -> int {
    set c = 0;
    while (x != 0) { x &= x - 1; c++; }
    return c;
}

fn reverse_bits(x: uint) -> uint {
    set r: uint = 0;
    for (set i = 0; i < 32; i++) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

// sieve over a bitset held in uint words: bit k of word k >> 5
fn bit_sieve(n: int) -> int {
    set words = (n >> 5) + 1;
    set bits: uint[] = [0; words];
    set count = 0;
    for (set i = 2; i < n; i++) {
        if (((bits[i >> 5] >> uint(i & 31)) & 1) == 0) {   // (& binds looser than ==)
            count++;
            for (set j = i * 2; j < n; j += i) { bits[j >> 5] |= 1 << uint(j & 31); }
        }
    }
    return count;
}

fn crc32(data: byte[]) -> uint {
    set crc: uint = 0xFFFFFFFF;
    for (set i = 0; i < len(data); i++) {
        crc ^= uint(data[i]);
        for (set k = 0; k < 8; k++) {
            set mask: uint = 0 - (crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }
    return ~crc;
}

// signed shifts: arithmetic right shift of negatives, multiply / divide by powers of two
fn signed_mix(n: int) -> int {
    set acc = 0;
    for (set i = -n; i < n; i++) {
        acc += (i >> 3) + ((i << 2) >> 1) - (i >> (i & 7));
    }
    return acc;
}

fn byte_rotations(n: int) -> int {
    set b: byte = 0x5A;
    set acc = 0;
    for (set i = 0; i < n; i++) {
        set k: byte = byte(i & 7);
        b = (b << k) | (b >> (byte(8) - k) & 0xFF);
        acc += int(b);
    }
    return acc;
}

fn main() {
    set h: uint = 0;
    for (set i = 0; i < 18000; i++) { h ^= reverse_bits(xorshift32()) >> 3; }
    print(h); print("\n");

    set pc = 0;
    for (set i = 0; i < 36000; i++) { pc += popcount(xorshift32()); }
    print(pc); print("\n");

    print(bit_sieve(150000)); print("\n");

    set text: byte[] = [0; 6000];
    for (set i = 0; i < len(text); i++) { text[i] = byte(xorshift32() >> 24); }
    print(crc32(text)); print("\n");

    print(signed_mix(60000)); print("\n");
    print(byte_rotations(80000)); print("\n");

    set masks = 0;
    for (set i = 0; i < 120000; i++) { masks += (1 << (i & 15)) >> 2; }
    print(masks); print("\n");
}
