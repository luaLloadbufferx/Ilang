// PGO training: arrays of arrays (int[][], float[][]), ARRREP / ARRCLONE / ARRFREE, row access chains g[i][j].
fn make_int_matrix(n: int, seed: int) -> int[][] {
    set row: int[] = [0; 48];
    set m: int[][] = [row; 48];
    for (set i = 0; i < n; i = i + 1) {
        for (set j = 0; j < n; j = j + 1) { m[i][j] = (i * seed + j * 7 + 3) % 17; }
    }
    return m;
}

fn matmul_int(a: int[][], b: int[][], c: int[][], n: int) {
    for (set i = 0; i < n; i = i + 1) {
        for (set j = 0; j < n; j = j + 1) {
            set s = 0;
            for (set k = 0; k < n; k = k + 1) {
                set x = a[i][k];
                set y = b[k][j];
                s = s + x * y;
            }
            c[i][j] = s;
        }
    }
}

fn trace_int(m: int[][], n: int) -> int {
    set t = 0;
    for (set i = 0; i < n; i = i + 1) { t = t + m[i][i]; }
    return t;
}

fn matmul_float(n: int) -> float {
    set row: float[] = [0.0; 32];
    set a: float[][] = [row; 32];
    set b: float[][] = [row; 32];
    set c: float[][] = [row; 32];
    for (set i = 0; i < n; i = i + 1) {
        for (set j = 0; j < n; j = j + 1) {
            a[i][j] = float(i + j) * 0.25;
            b[i][j] = float(i) - float(j) * 0.5;
        }
    }
    for (set i = 0; i < n; i = i + 1) {
        for (set j = 0; j < n; j = j + 1) {
            set s: float = 0.0;
            for (set k = 0; k < n; k = k + 1) {
                set x = a[i][k];
                set y = b[k][j];
                s = s + x * y;
            }
            c[i][j] = s;
        }
    }
    set t: float = 0.0;
    for (set i = 0; i < n; i = i + 1) { t = t + c[i][i]; }
    return t;
}

fn transpose(m: int[][], n: int) -> int[][] {
    set t = copy(m);
    for (set i = 0; i < n; i = i + 1) {
        for (set j = 0; j < n; j = j + 1) { t[j][i] = m[i][j]; }
    }
    return t;
}

fn life_step(cur: int[][], nxt: int[][], n: int) -> int {
    set alive = 0;
    for (set i = 1; i < n - 1; i = i + 1) {
        for (set j = 1; j < n - 1; j = j + 1) {
            set c = cur[i - 1][j - 1] + cur[i - 1][j] + cur[i - 1][j + 1]
                  + cur[i][j - 1] + cur[i][j + 1]
                  + cur[i + 1][j - 1] + cur[i + 1][j] + cur[i + 1][j + 1];
            if (cur[i][j] == 1) {
                if (c == 2 || c == 3) { nxt[i][j] = 1; alive = alive + 1; } else { nxt[i][j] = 0; }
            } else {
                if (c == 3) { nxt[i][j] = 1; alive = alive + 1; } else { nxt[i][j] = 0; }
            }
        }
    }
    return alive;
}

fn life(g: int, gens: int) -> int {
    set row: int[] = [0; 40];
    set cur: int[][] = [row; 40];
    set nxt: int[][] = [row; 40];
    for (set i = 0; i < g; i = i + 1) {
        for (set j = 0; j < g; j = j + 1) {
            set h = i * 7 + j * 13 + i * j;
            if (h % 5 == 0) { cur[i][j] = 1; }
        }
    }
    set total = 0;
    for (set s = 0; s < gens; s = s + 1) {
        total = total + life_step(cur, nxt, g);
        for (set i = 0; i < g; i = i + 1) {
            for (set j = 0; j < g; j = j + 1) {
                set v = nxt[i][j];
                cur[i][j] = v;
            }
        }
    }
    return total;
}

// build, copy and drop many small arrays of arrays
fn churn(n: int) -> int {
    set acc = 0;
    for (set k = 0; k < n; k = k + 1) {
        set w16 = 16;
        set r2: int[] = [k; w16];
        set grid: int[][] = [r2; 8];
        set gi = k % 8;
        set gj = k % 16;
        grid[gi][gj] = k * 2;
        set dup = copy(grid);
        set dv = dup[gi][gj];
        acc = acc + dv + len(dup) + len(dup[0]);
    }
    return acc;
}

fn main() {
    set n = 32;
    set a = make_int_matrix(n, 5);
    set b = make_int_matrix(n, 3);
    set c = make_int_matrix(n, 1);
    for (set r = 0; r < 6; r = r + 1) {
        matmul_int(a, b, c, n);
        for (set i = 0; i < n; i = i + 1) {
            for (set j = 0; j < n; j = j + 1) {
                set v = c[i][j];
                a[i][j] = v % 19;
            }
        }
    }
    print(trace_int(a, n)); print("\n");

    print(matmul_float(24)); print("\n");

    set t = transpose(a, n);
    print(trace_int(t, n)); print(t[3][5] - a[5][3]); print("\n");

    print(life(40, 40)); print("\n");
    print(churn(400)); print("\n");
}
