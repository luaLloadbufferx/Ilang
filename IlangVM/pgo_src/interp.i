// PGO training: an interpreter written in Ilang (branch-heavy: long if/else-if chains, a stack in an int[], a tape in a byte[]).
// Runs a Brainfuck program and a small stack-machine program.
fn bf_run(prog: string, limit: int) -> int {
    set tape: byte[] = [0; 512];
    set jump: int[] = [0; 512];
    set stack: int[] = [0; 64];
    set sp = 0;
    set n = len(prog);
    for (set i = 0; i < n; i = i + 1) {
        set c = prog[i];
        if (c == 91) { stack[sp] = i; sp = sp + 1; }
        else if (c == 93) {
            sp = sp - 1;
            set j = stack[sp];
            jump[i] = j;
            jump[j] = i;
        }
    }
    set ip = 0;
    set dp = 0;
    set steps = 0;
    set outsum = 0;
    while (ip < n && steps < limit) {
        set c = prog[ip];
        if (c == 62) { dp = dp + 1; if (dp >= 512) { dp = 0; } }
        else if (c == 60) { dp = dp - 1; if (dp < 0) { dp = 511; } }
        else if (c == 43) { tape[dp] = tape[dp] + byte(1); }
        else if (c == 45) { tape[dp] = tape[dp] - byte(1); }
        else if (c == 46) { outsum = outsum * 3 + int(tape[dp]); outsum = outsum % 1000003; }
        else if (c == 91) { if (tape[dp] == byte(0)) { ip = jump[ip]; } }
        else if (c == 93) { if (tape[dp] != byte(0)) { ip = jump[ip]; } }
        ip = ip + 1;
        steps = steps + 1;
    }
    return outsum * 10 + steps % 10;
}

// stack machine: 0 push imm, 1 add, 2 mul, 3 dup, 4 swap, 5 pop, 6 jnz target, 7 dec top, 8 halt, 9 sub, 10 mod
fn sm_run(code: int[], n: int, limit: int) -> int {
    set st: int[] = [0; 64];
    set sp = 0;
    set pc = 0;
    set steps = 0;
    while (pc < n && steps < limit) {
        set op = code[pc];
        if (op == 0) { st[sp] = code[pc + 1]; sp = sp + 1; pc = pc + 2; }
        else if (op == 1) { sp = sp - 1; st[sp - 1] = st[sp - 1] + st[sp]; pc = pc + 1; }
        else if (op == 2) { sp = sp - 1; st[sp - 1] = st[sp - 1] * st[sp] % 1000003; pc = pc + 1; }
        else if (op == 3) { st[sp] = st[sp - 1]; sp = sp + 1; pc = pc + 1; }
        else if (op == 4) { set t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; pc = pc + 1; }
        else if (op == 5) { sp = sp - 1; pc = pc + 1; }
        else if (op == 6) { if (st[sp - 1] != 0) { pc = code[pc + 1]; } else { pc = pc + 2; } }
        else if (op == 7) { st[sp - 1] = st[sp - 1] - 1; pc = pc + 1; }
        else if (op == 8) { break; }
        else if (op == 9) { sp = sp - 1; st[sp - 1] = st[sp - 1] - st[sp]; pc = pc + 1; }
        else if (op == 10) { sp = sp - 1; st[sp - 1] = st[sp - 1] % st[sp]; pc = pc + 1; }
        else { break; }
        steps = steps + 1;
    }
    return st[0] * 7 + st[1] + steps;
}

fn main() {
    // 10 x 10 nested counting loops, a squares printer and some pointer moves
    set bf = "++++++++++[>++++++++++[>+++++<-]<-]>>[-]++++++++[>++++++++<-]>.<++++++++++[>++++++[>++<-]<-]>>.";
    set sum = 0;
    for (set r = 0; r < 40; r = r + 1) { sum = sum + bf_run(bf, 20000); }
    print(sum); print("\n");

    // acc = 1; counter = 3000; loop { acc = (acc * 3 + 7) % 1000003; counter = counter - 1 } until counter == 0
    set code: int[] = [0, 1, 0, 3000, 4, 0, 3, 2, 0, 7, 1, 0, 1000003, 10, 4, 7, 6, 4, 8];
    set m = 0;
    for (set r = 0; r < 8; r = r + 1) { m = m + sm_run(code, 19, 40000); }
    print(m); print("\n");
}
