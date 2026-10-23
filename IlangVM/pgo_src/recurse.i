// PGO training: deep and wide calls. Recursion (n-queens, permutations, Ackermann, memoised Fibonacci, mutual
// recursion), functions with more arguments than registers (stack arguments), ?:, compound assignment. Depths stay
// well inside the VM's 320-slot data stack (every frame pushes the registers it saves).
fn queens(row: int, n: int, cols: bool[], d1: bool[], d2: bool[]) -> int {
    if (row == n) { return 1; }
    set count = 0;
    for (set c = 0; c < n; c++) {
        if (!cols[c] && !d1[row + c] && !d2[row - c + n - 1]) {
            cols[c] = true; d1[row + c] = true; d2[row - c + n - 1] = true;
            count += queens(row + 1, n, cols, d1, d2);
            cols[c] = false; d1[row + c] = false; d2[row - c + n - 1] = false;
        }
    }
    return count;
}

fn permute(a: int[], k: int) -> int {
    if (k == len(a)) { return a[0] * 3 + a[len(a) - 1]; }
    set s = 0;
    for (set i = k; i < len(a); i++) {
        set t = a[k]; a[k] = a[i]; a[i] = t;
        s += permute(a, k + 1);
        t = a[k]; a[k] = a[i]; a[i] = t;
    }
    return s;
}

fn ack(m: int, n: int) -> int {
    if (m == 0) { return n + 1; }
    if (n == 0) { return ack(m - 1, 1); }
    return ack(m - 1, ack(m, n - 1));
}

fn fib(n: int, memo: int[]) -> int {
    if (n < 2) { return n; }
    if (memo[n] != 0) { return memo[n]; }
    memo[n] = (fib(n - 1, memo) + fib(n - 2, memo)) % 1000000007;
    return memo[n];
}

fn is_even(n: int) -> bool { return n == 0 ? true : is_odd(n - 1); }
fn is_odd(n: int) -> bool { return n == 0 ? false : is_even(n - 1); }

// 20 arguments: with the default register split some of them always travel on the stack
fn wide(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19) -> int {
    return a0 - a1 + a2 - a3 + a4 - a5 + a6 - a7 + a8 - a9 + a10 - a11 + a12 - a13 + a14 - a15 + a16 - a17 + a18 - a19;
}

fn tree_sum(depth: int, v: int) -> int {
    return depth == 0 ? v : tree_sum(depth - 1, v * 2) + tree_sum(depth - 1, v * 2 + 1);
}

fn main() {
    set n = 9;
    print(queens(0, n, [false; n], [false; 2 * n], [false; 2 * n])); print("\n");
    print(permute([1, 2, 3, 4, 5, 6, 7, 8], 0)); print("\n");
    set ak = 0;
    for (set i = 0; i < 40; i++) { ak += ack(2, 30 + i % 10) + ack(3, 3); }
    print(ak); print("\n");
    set memo = [0; 100];
    set f = 0;
    for (set i = 0; i < 3000; i++) { memo = [0; 100]; f = (f + fib(60 + i % 30, memo)) % 1000000007; }
    print(f); print("\n");
    set ev = 0;
    for (set i = 0; i < 4000; i++) { ev += is_even(i % 90) ? 1 : 0; }
    print(ev); print("\n");
    set w = 0;
    for (set i = 0; i < 60000; i++) { w += wide(i, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, i & 7); }
    print(w); print("\n");
    print(tree_sum(17, 1)); print("\n");
}
