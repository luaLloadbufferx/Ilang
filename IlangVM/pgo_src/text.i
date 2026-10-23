// PGO training: string-processing. Concatenation, comparison (STRCMP), find, substr, chr, str(), number parsing
// (int() / float() / is_int()), per-byte loops over strings, building and tearing down many temporaries.
fn repeat(s: string, n: int) -> string {
    set out = "";
    for (set i = 0; i < n; i = i + 1) { out = out + s; }
    return out;
}

fn count_char(s: string, c: byte) -> int {
    set n = 0;
    for (set i = 0; i < len(s); i = i + 1) { if (s[i] == c) { n = n + 1; } }
    return n;
}

fn reverse(s: string) -> string {
    set out = "";
    set i = len(s) - 1;
    while (i >= 0) {
        out = out + chr(s[i]);
        i = i - 1;
    }
    return out;
}

fn is_palindrome(s: string) -> bool {
    set i = 0;
    set j = len(s) - 1;
    while (i < j) {
        if (s[i] != s[j]) { return false; }
        i = i + 1;
        j = j - 1;
    }
    return true;
}

fn caesar(s: string, k: int) -> string {
    set out = "";
    for (set i = 0; i < len(s); i = i + 1) {
        set c = int(s[i]);
        if (c >= 97 && c <= 122) { c = (c - 97 + k) % 26 + 97; }
        out = out + chr(byte(c));
    }
    return out;
}

fn split_words(s: string) -> int {
    // counts words and the sum of their lengths without building arrays of strings
    set words = 0;
    set start = 0;
    set total = 0;
    for (set i = 0; i <= len(s); i = i + 1) {
        if (i == len(s) || s[i] == 32) {
            if (i > start) {
                set w = substr(s, start, i - start);
                words = words + 1;
                total = total + len(w);
            }
            start = i + 1;
        }
    }
    return words * 1000 + total;
}

fn parse_sum(n: int) -> int {
    set acc = 0;
    for (set i = 0; i < n; i = i + 1) {
        set t = str(i * 37 % 1000);
        if (is_int(t)) { acc = acc + int(t); }
        set f = str(float(i % 50) * 0.5);
        if (is_float(f)) { acc = acc + int(float(f)); }
    }
    return acc;
}

fn compare_all(n: int) -> int {
    set less = 0;
    set eq = 0;
    for (set i = 0; i < n; i = i + 1) {
        set a = "item" + str(i % 40);
        set b = "item" + str((i * 7) % 40);
        if (a < b) { less = less + 1; }
        if (a == b) { eq = eq + 1; }
        if (a > b) { less = less - 1; }
    }
    return less * 100 + eq;
}

fn main() {
    set text = repeat("the quick brown fox jumps over the lazy dog ", 40);
    print(len(text)); print("\n");
    print(count_char(text, 111)); print("\n");

    set words = 0;
    for (set r = 0; r < 30; r = r + 1) { words = words + split_words(text); }
    print(words); print("\n");

    set h = 0;
    for (set r = 0; r < 40; r = r + 1) {
        set enc = caesar(text, r % 25 + 1);
        h = h + count_char(enc, 97);
        set back = caesar(enc, 26 - (r % 25 + 1));
        if (back == text) { h = h + 1; }
    }
    print(h); print("\n");

    set pal = 0;
    for (set i = 0; i < 1500; i = i + 1) {
        set s = str(i) + reverse(str(i));
        if (is_palindrome(s)) { pal = pal + 1; }
        if (is_palindrome(str(i))) { pal = pal + 1; }
    }
    print(pal); print("\n");

    print(parse_sum(8000)); print("\n");
    print(compare_all(12000)); print("\n");

    // find / substr scanning
    set hits = 0;
    set pos = 0;
    for (set r = 0; r < 50; r = r + 1) {
        pos = 0;
        while (pos < len(text)) {
            set rest = substr(text, pos, len(text) - pos);
            set f = find(rest, "fox");
            if (f < 0) { break; }
            hits = hits + 1;
            pos = pos + f + 3;
        }
    }
    print(hits); print("\n");
}
