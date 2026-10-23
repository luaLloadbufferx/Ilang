// PGO training: global variables (scalars read and written from many functions, global arrays changed in place),
// structs with int / float / string fields made and freed in loops, set const.
set const W = 64;
set const H = 48;
set heat: float[] = [0.0; 3072];
set next_heat: float[] = [0.0; 3072];
set steps = 0;
set total_energy = 0.0;

struct Particle {
    name: string;
    x: float;
    y: float;
    vx: float;
    vy: float;
    hits: int;
}

fn idx(x: int, y: int) -> int { return y * W + x; }

fn diffuse() {
    for (set y = 1; y < H - 1; y++) {
        for (set x = 1; x < W - 1; x++) {
            next_heat[idx(x, y)] = (heat[idx(x - 1, y)] + heat[idx(x + 1, y)] + heat[idx(x, y - 1)] + heat[idx(x, y + 1)]) * 0.25;
        }
    }
    total_energy = 0.0;
    for (set i = 0; i < W * H; i++) { heat[i] = next_heat[i]; total_energy += heat[i]; }
    steps++;
}

fn make(i: int) -> Particle {
    set p = Particle();
    p.name = "p" + str(i);
    p.x = float(i % W); p.y = float(i % H);
    p.vx = float(i % 5) - 2.0; p.vy = float(i % 3) - 1.0;
    return p;
}

fn move(p: Particle) {
    p.x += p.vx; p.y += p.vy;
    if (p.x < 0.0 || p.x >= float(W)) { p.vx = -p.vx; p.x += p.vx; p.hits++; }
    if (p.y < 0.0 || p.y >= float(H)) { p.vy = -p.vy; p.y += p.vy; p.hits++; }
    heat[idx(int(p.x), int(p.y))] += 1.0;
}

fn main() {
    set hits = 0;
    set names = 0;
    for (set round = 0; round < 40; round++) {
        for (set i = 0; i < 40; i++) {
            set p = make(round * 40 + i);
            for (set k = 0; k < 30; k++) { move(p); }
            hits += p.hits;
            names += len(p.name);
        }   // every particle (and its name string) is freed here
        diffuse();
    }
    print(steps); print(" "); print(hits); print(" "); print(names); print("\n");
    print(total_energy); print("\n");
}
