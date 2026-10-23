// PGO training: dynamic programming over int[] tables - LCS, edit distance, knapsack, coin change, longest increasing subsequence,
// prefix sums. Lots of min/max selection, 2-D indexing flattened into one array, nested loops with early exits.
fn max2(a: int, b: int) -> int { if (a > b) { return a; } return b; }
fn min2(a: int, b: int) -> int { if (a < b) { return a; } return b; }
fn min3(a: int, b: int, c: int) -> int { return min2(a, min2(b, c)); }

fn lcs(a: byte[], n: int, b: byte[], m: int) -> int {
    set w = m + 1;
    set t: int[] = [0; 6000];
    for (set i = 1; i <= n; i = i + 1) {
        for (set j = 1; j <= m; j = j + 1) {
            if (a[i - 1] == b[j - 1]) {
                t[i * w + j] = t[(i - 1) * w + (j - 1)] + 1;
            } else {
                t[i * w + j] = max2(t[(i - 1) * w + j], t[i * w + (j - 1)]);
            }
        }
    }
    return t[n * w + m];
}

fn edit_distance(a: byte[], n: int, b: byte[], m: int) -> int {
    set w = m + 1;
    set t: int[] = [0; 6000];
    for (set i = 0; i <= n; i = i + 1) { t[i * w] = i; }
    for (set j = 0; j <= m; j = j + 1) { t[j] = j; }
    for (set i = 1; i <= n; i = i + 1) {
        for (set j = 1; j <= m; j = j + 1) {
            set cost = 1;
            if (a[i - 1] == b[j - 1]) { cost = 0; }
            set del = t[(i - 1) * w + j] + 1;
            set ins = t[i * w + (j - 1)] + 1;
            set sub = t[(i - 1) * w + (j - 1)] + cost;
            t[i * w + j] = min3(del, ins, sub);
        }
    }
    return t[n * w + m];
}

fn knapsack(items: int, cap: int) -> int {
    set wt: int[] = [0; 64];
    set val: int[] = [0; 64];
    for (set i = 0; i < items; i = i + 1) { wt[i] = (i * 7) % 23 + 3; val[i] = (i * 13) % 37 + 5; }
    set best: int[] = [0; 1024];
    for (set i = 0; i < items; i = i + 1) {
        set c = cap;
        while (c >= wt[i]) {
            set cand = best[c - wt[i]] + val[i];
            if (cand > best[c]) { best[c] = cand; }
            c = c - 1;
        }
    }
    return best[cap];
}

fn coin_change(amount: int) -> int {
    set coins: int[] = [1, 5, 10, 25, 50, 100];
    set ways: int[] = [0; 2048];
    ways[0] = 1;
    for (set k = 0; k < 6; k = k + 1) {
        set c = coins[k];
        for (set a = c; a <= amount; a = a + 1) {
            ways[a] = (ways[a] + ways[a - c]) % 1000003;
        }
    }
    return ways[amount];
}

fn lis(n: int) -> int {
    set a: int[] = [0; 400];
    set d: int[] = [0; 400];
    for (set i = 0; i < n; i = i + 1) { a[i] = (i * 7919 + 13) % 1009; }
    set best = 0;
    for (set i = 0; i < n; i = i + 1) {
        d[i] = 1;
        for (set j = 0; j < i; j = j + 1) {
            if (a[j] < a[i] && d[j] + 1 > d[i]) { d[i] = d[j] + 1; }
        }
        best = max2(best, d[i]);
    }
    return best;
}

fn prefix(n: int) -> int {
    set a: int[] = [0; n];
    for (set i = 0; i < n; i = i + 1) { a[i] = (i * 31 + 7) % 101; }
    set np1 = n + 1;
    set p: int[] = [0; np1];
    for (set i = 0; i < n; i = i + 1) { p[i + 1] = p[i] + a[i]; }
    set best = 0;
    for (set i = 0; i < n; i = i + 7) {
        for (set j = i + 1; j <= n; j = j + 97) { best = max2(best, p[j] - p[i]); }
    }
    return best;
}

fn main() {
    set a: byte[] = [0; 80];
    set b: byte[] = [0; 70];
    for (set i = 0; i < 80; i = i + 1) { a[i] = byte(97 + (i * 5 + i / 3) % 6); }
    for (set i = 0; i < 70; i = i + 1) { b[i] = byte(97 + (i * 7 + i / 4) % 6); }
    set s = 0;
    for (set r = 0; r < 12; r = r + 1) {
        s = s + lcs(a, 80, b, 70);
        s = s + edit_distance(a, 80, b, 70);
    }
    print(s); print("\n");
    print(knapsack(60, 1000)); print("\n");
    set cc = 0;
    for (set r = 0; r < 6; r = r + 1) { cc = cc + coin_change(2000); }
    print(cc); print("\n");
    print(lis(400)); print("\n");
    print(prefix(2000)); print("\n");
}
