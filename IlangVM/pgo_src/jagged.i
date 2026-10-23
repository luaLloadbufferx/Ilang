// PGO training: arrays whose size is known only at run time (NEWARRAY), arrays of arrays (jagged, 3-D), [row; n]
// (ARRREP), copy() (ARRCLONE), replacing rows (ARRFREE of the old one), row reads and writes.
fn pascal(n: int) -> int[][] {
    set t: int[][] = [[0; 0]; n];
    for (set i = 0; i < n; i++) {
        t[i] = [1; i + 1];
        for (set j = 1; j < i; j++) { t[i][j] = (t[i - 1][j - 1] + t[i - 1][j]) % 1000007; }
    }
    return t;
}

fn sum_all(g: int[][]) -> int {
    set s = 0;
    for (set i = 0; i < len(g); i++) {
        set row = g[i];
        for (set j = 0; j < len(row); j++) { s = (s + row[j]) % 1000007; }
    }
    return s;
}

fn life_step(cur: int[][], nxt: int[][], n: int) {
    for (set i = 0; i < n; i++) {
        for (set j = 0; j < n; j++) {
            set c = 0;
            for (set di = -1; di <= 1; di++) {
                for (set dj = -1; dj <= 1; dj++) {
                    if (di != 0 || dj != 0) { c += cur[(i + di + n) % n][(j + dj + n) % n]; }
                }
            }
            nxt[i][j] = (c == 3 || (c == 2 && cur[i][j] == 1)) ? 1 : 0;
        }
    }
}

fn cube_sum(n: int) -> int {
    set c: int[][][] = [[[0; n]; n]; n];
    for (set i = 0; i < n; i++) { for (set j = 0; j < n; j++) { for (set k = 0; k < n; k++) { c[i][j][k] = i * j + k; } } }
    set s = 0;
    for (set i = 0; i < n; i++) { for (set j = 0; j < n; j++) { for (set k = 0; k < n; k++) { s += c[i][j][k]; } } }
    return s;
}

fn main() {
    set p = pascal(260);
    print(sum_all(p)); print("\n");

    set n = 48;
    set a: int[][] = [[0; n]; n];
    a[1][2] = 1; a[2][3] = 1; a[3][1] = 1; a[3][2] = 1; a[3][3] = 1;    // a glider
    a[20][20] = 1; a[20][21] = 1; a[20][22] = 1;                         // a blinker
    set b: int[][] = copy(a);
    for (set gen = 0; gen < 40; gen++) {
        if (gen % 2 == 0) { life_step(a, b, n); } else { life_step(b, a, n); }
    }
    print(sum_all(a)); print("\n");

    // rows replaced over and over, of growing and shrinking length
    set rows: float[][] = [[0.0; 1]; 64];
    set total = 0.0;
    for (set r = 0; r < 4000; r++) {
        set k = r % 64;
        rows[k] = [float(r); (r * 7) % 50 + 1];
        total += rows[k][len(rows[k]) - 1];
    }
    print(total); print("\n");

    set seed = [1, 2, 3, 4, 5, 6, 7, 8];
    set reps = 0;
    for (set r = 0; r < 600; r++) {
        set m: int[][] = [seed; 40];
        m[r % 40][r % 8] = r;
        reps += m[r % 40][r % 8] + len(m);
    }
    print(reps); print("\n");

    print(cube_sum(26)); print("\n");
}
