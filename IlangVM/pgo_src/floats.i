// PGO training: float arithmetic and comparisons, int<->float casts, uint/byte arithmetic, a float[] workload.
struct Body { x: float; y: float; vx: float; vy: float; m: float; }

set steps_done = 0;

fn mandel(w: int, h: int, maxit: int) -> int {
    set inside = 0;
    for (set py = 0; py < h; py = py + 1) {
        for (set px = 0; px < w; px = px + 1) {
            set cx: float = float(px) / float(w) * 3.5 - 2.5;
            set cy: float = float(py) / float(h) * 2.0 - 1.0;
            set zx: float = 0.0;
            set zy: float = 0.0;
            set it = 0;
            while (it < maxit && zx * zx + zy * zy <= 4.0) {
                set t: float = zx * zx - zy * zy + cx;
                zy = 2.0 * zx * zy + cy;
                zx = t;
                it = it + 1;
            }
            if (it == maxit) { inside = inside + 1; }
        }
    }
    return inside;
}

fn newton_sqrt(v: float) -> float {
    set x: float = v;
    if (x < 1.0) { x = 1.0; }
    for (set i = 0; i < 12; i = i + 1) {
        x = (x + v / x) * 0.5;
    }
    return x;
}

fn integrate(n: int) -> float {
    // trapezoid rule for x*x*x - 2*x over [0, 3]
    set h: float = 3.0 / float(n);
    set s: float = 0.0;
    for (set i = 0; i < n; i = i + 1) {
        set x: float = float(i) * h;
        set y: float = x * x * x - 2.0 * x;
        set x2: float = x + h;
        set y2: float = x2 * x2 * x2 - 2.0 * x2;
        s = s + (y + y2) * 0.5 * h;
    }
    return s;
}

fn nbody(steps: int) -> float {
    set b0 = Body(); b0.x = 0.0;  b0.y = 0.0;  b0.vx = 0.0;  b0.vy = 0.0;  b0.m = 100.0;
    set b1 = Body(); b1.x = 10.0; b1.y = 0.0;  b1.vx = 0.0;  b1.vy = 3.0;  b1.m = 1.0;
    set b2 = Body(); b2.x = -6.0; b2.y = 2.0;  b2.vx = 1.0;  b2.vy = -4.0; b2.m = 2.0;
    set dt: float = 0.01;
    for (set s = 0; s < steps; s = s + 1) {
        // b1 and b2 orbit b0, and attract each other a little
        set dx: float = b0.x - b1.x;
        set dy: float = b0.y - b1.y;
        set d2: float = dx * dx + dy * dy + 0.01;
        set inv: float = 1.0 / (d2 * newton_sqrt(d2));
        b1.vx = b1.vx + dx * b0.m * inv * dt;
        b1.vy = b1.vy + dy * b0.m * inv * dt;

        dx = b0.x - b2.x;
        dy = b0.y - b2.y;
        d2 = dx * dx + dy * dy + 0.01;
        inv = 1.0 / (d2 * newton_sqrt(d2));
        b2.vx = b2.vx + dx * b0.m * inv * dt;
        b2.vy = b2.vy + dy * b0.m * inv * dt;

        dx = b1.x - b2.x;
        dy = b1.y - b2.y;
        d2 = dx * dx + dy * dy + 0.5;
        inv = 1.0 / (d2 * newton_sqrt(d2));
        b1.vx = b1.vx - dx * b2.m * inv * dt;
        b1.vy = b1.vy - dy * b2.m * inv * dt;
        b2.vx = b2.vx + dx * b1.m * inv * dt;
        b2.vy = b2.vy + dy * b1.m * inv * dt;

        b1.x = b1.x + b1.vx * dt;
        b1.y = b1.y + b1.vy * dt;
        b2.x = b2.x + b2.vx * dt;
        b2.y = b2.y + b2.vy * dt;
        steps_done = steps_done + 1;
    }
    return b1.x + b1.y + b2.x + b2.y;
}

fn dot_and_scale(n: int, rounds: int) -> float {
    set a: float[] = [0.0; 256];
    set b: float[] = [0.0; 256];
    for (set i = 0; i < 256; i = i + 1) {
        a[i] = float(i) * 0.5 + 1.0;
        b[i] = 1.0 / float(i + 1);
    }
    set total: float = 0.0;
    for (set r = 0; r < rounds; r = r + 1) {
        set s: float = 0.0;
        for (set i = 0; i < n; i = i + 1) {
            s = s + a[i] * b[i];
            a[i] = a[i] * 0.999 + 0.001;
        }
        total = total + s;
    }
    return total;
}

fn mixed_ints(n: int) -> int {
    // uint and byte arithmetic, casts between them
    set u: uint = 12345;
    set chk: uint = 0;
    set bt: byte = byte(7);
    for (set i = 0; i < n; i = i + 1) {
        u = u * 1103515245 + 12345;
        chk = chk + u / 65536;
        chk = chk - u % 977;
        bt = byte(int(bt) + i % 5);
        if (u > 2000000000) { chk = chk + 1; }
    }
    return int(chk % 100000) + int(bt);
}

fn main() {
    print(mandel(70, 36, 120)); print("\n");
    print(newton_sqrt(2.0)); print("\n");
    print(integrate(60000)); print("\n");
    print(nbody(20000)); print("\n");
    print(steps_done); print("\n");
    print(dot_and_scale(256, 400)); print("\n");
    print(mixed_ints(150000)); print("\n");
}
