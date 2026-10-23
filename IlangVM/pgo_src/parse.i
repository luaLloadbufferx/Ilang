// PGO training: parsing text. A recursive-descent calculator over generated expressions, a CSV-like line splitter,
// int() / float() / is_int() / is_float(), find, substr, str, chr, byte-by-byte scanning, string comparison.
set pos = 0;

fn peek(s: string) -> byte { return pos < len(s) ? s[pos] : byte(0); }

fn skip_spaces(s: string) { while (pos < len(s) && s[pos] == ' ') { pos++; } }

fn number(s: string) -> int {
    set start = pos;
    while (pos < len(s) && s[pos] >= '0' && s[pos] <= '9') { pos++; }
    return int(substr(s, start, pos - start));
}

fn factor(s: string) -> int {
    skip_spaces(s);
    if (peek(s) == '(') {
        pos++;
        set v = expr(s);
        skip_spaces(s);
        pos++;   // ')'
        return v;
    }
    if (peek(s) == '-') { pos++; return -factor(s); }
    return number(s);
}

fn term(s: string) -> int {
    set v = factor(s);
    for (;;) {
        skip_spaces(s);
        set c = peek(s);
        if (c == '*') { pos++; v = v * factor(s) % 1000003; }
        else if (c == '%') { pos++; set d = factor(s); v = d == 0 ? v : v % d; }
        else { return v; }
    }
    return v;
}

fn expr(s: string) -> int {
    set v = term(s);
    for (;;) {
        skip_spaces(s);
        set c = peek(s);
        if (c == '+') { pos++; v += term(s); }
        else if (c == '-') { pos++; v -= term(s); }
        else { return v; }
    }
    return v;
}

fn eval(s: string) -> int { pos = 0; return expr(s); }

set seed = 7;
fn rnd(n: int) -> int { seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF; return seed % n; }

fn gen(depth: int) -> string {
    if (depth == 0 || rnd(4) == 0) { return str(rnd(1000)); }
    set ops = "+-*%";
    set op = chr(ops[rnd(4)]);
    if (rnd(3) == 0) { return "(" + gen(depth - 1) + " " + op + " " + gen(depth - 1) + ")"; }
    return gen(depth - 1) + " " + op + " " + gen(depth - 1);
}

// "name;12;3.5;yes" -> a checksum over the typed fields
fn record(line: string) -> int {
    set sum = 0;
    set rest = copy(line);
    set field = 0;
    while (len(rest) > 0) {
        set cut = find(rest, ";");
        set f = cut < 0 ? copy(rest) : substr(rest, 0, cut);
        if (is_int(f)) { sum += int(f) * (field + 1); }
        else if (is_float(f)) { sum += int(float(f) * 10.0); }
        else if (f == "yes") { sum += 1000; }
        else { sum += len(f); }
        rest = cut < 0 ? "" : substr(rest, cut + 1, len(rest) - cut - 1);
        field++;
    }
    return sum;
}

fn main() {
    set total = 0;
    set chars = 0;
    for (set i = 0; i < 700; i++) {
        set e = gen(6);
        chars += len(e);
        total = (total + eval(e)) % 1000003;
    }
    print(total); print(" "); print(chars); print("\n");

    set names = "alpha beta gamma delta epsilon";
    set acc = 0;
    for (set i = 0; i < 6000; i++) {
        set line = substr(names, (i * 6) % 25, 5) + ";" + str(i) + ";" + str(float(i) / 4.0) + ";" + (i % 3 == 0 ? "yes" : "no");
        acc = (acc + record(line)) % 1000003;
    }
    print(acc); print("\n");

    set words = 0;
    set text = "the quick brown fox jumps over the lazy dog and keeps running far away from the hound";
    for (set r = 0; r < 600; r++) {
        set i = 0;
        while (i < len(text)) {
            while (i < len(text) && text[i] == ' ') { i++; }
            set start = i;
            while (i < len(text) && text[i] != ' ') { i++; }
            if (i > start && substr(text, start, i - start) < "m") { words++; }
        }
    }
    print(words); print("\n");
}
