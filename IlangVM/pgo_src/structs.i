// PGO training: structs on the VM heap (RESERVEMEM / FREEMEM, fields at several offsets, string fields), structs passed to and
// returned from functions, array-backed binary search tree and linked list, a global struct.
struct Vec { x: float; y: float; z: float; }
struct Account { id: int; balance: int; active: bool; level: byte; name: string; }
struct Stats { hits: int; misses: int; last: int; }

set st = Stats();

fn vadd(a: Vec, b: Vec) -> Vec {
    set r = Vec();
    r.x = a.x + b.x;
    r.y = a.y + b.y;
    r.z = a.z + b.z;
    return r;
}

fn vdot(a: Vec, b: Vec) -> float {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

fn vscale(a: Vec, k: float) {
    a.x = a.x * k;
    a.y = a.y * k;
    a.z = a.z * k;
}

fn make_account(id: int) -> Account {
    set a = Account();
    a.id = id;
    a.balance = id * 10;
    a.active = id % 3 != 0;
    a.level = byte(id % 7);
    a.name = "acct-" + str(id);
    return a;
}

fn apply(a: Account, delta: int) -> bool {
    if (!a.active) { st.misses = st.misses + 1; return false; }
    if (a.balance + delta < 0) { st.misses = st.misses + 1; return false; }
    a.balance = a.balance + delta;
    a.level = a.level + byte(1);
    st.hits = st.hits + 1;
    st.last = a.id;
    return true;
}

// array-backed binary search tree: node i has key[i], left[i], right[i]; index 0 means "none" (slot 0 is unused)
fn tree_build(keys: int[], left: int[], right: int[], n: int) -> int {
    set count = 1;
    set root = 1;
    keys[1] = n * 37 % 1009;
    count = 2;
    for (set i = 1; i < n; i = i + 1) {
        set k = (i * 7919 + 13) % 1009;
        set cur = root;
        set done = false;
        while (!done) {
            set ck = keys[cur];
            if (k == ck) { done = true; }
            else if (k < ck) {
                set l = left[cur];
                if (l == 0) { keys[count] = k; left[cur] = count; count = count + 1; done = true; }
                else { cur = l; }
            } else {
                set r = right[cur];
                if (r == 0) { keys[count] = k; right[cur] = count; count = count + 1; done = true; }
                else { cur = r; }
            }
        }
    }
    return count;
}

fn tree_depth(left: int[], right: int[], node: int) -> int {
    if (node == 0) { return 0; }
    set l = tree_depth(left, right, left[node]);
    set r = tree_depth(left, right, right[node]);
    if (l > r) { return l + 1; }
    return r + 1;
}

fn tree_sum(keys: int[], left: int[], right: int[], node: int) -> int {
    if (node == 0) { return 0; }
    set s = keys[node];
    s = s + tree_sum(keys, left, right, left[node]);
    s = s + tree_sum(keys, left, right, right[node]);
    return s;
}

fn tree_find(keys: int[], left: int[], right: int[], k: int) -> bool {
    set cur = 1;
    while (cur != 0) {
        set ck = keys[cur];
        if (k == ck) { return true; }
        if (k < ck) { cur = left[cur]; } else { cur = right[cur]; }
    }
    return false;
}

fn main() {
    // vectors: allocate, combine, free
    set acc = Vec();
    set unit = Vec();
    unit.x = 0.5; unit.y = 0.25; unit.z = 0.125;
    set dots: float = 0.0;
    for (set i = 0; i < 24000; i = i + 1) {
        set t = vadd(acc, unit);
        vscale(t, 0.999);
        dots = dots + vdot(t, unit);
        acc.x = t.x; acc.y = t.y; acc.z = t.z;
    }
    print(dots); print("\n");

    // accounts with string fields
    set total = 0;
    for (set i = 1; i < 6000; i = i + 1) {
        set a = make_account(i);
        if (apply(a, i % 11 - 5)) { total = total + a.balance; }
        apply(a, 0 - 100000);
        total = total + len(a.name) + int(a.level);
    }
    print(total); print("\n");
    print(st.hits); print(st.misses); print(st.last); print("\n");

    // the tree and a linked list in parallel arrays
    set keys: int[] = [0; 1100];
    set left: int[] = [0; 1100];
    set right: int[] = [0; 1100];
    set n = tree_build(keys, left, right, 1000);
    print(n); print("\n");
    set found = 0;
    for (set r = 0; r < 20; r = r + 1) {
        for (set k = 0; k < 1009; k = k + 1) { if (tree_find(keys, left, right, k)) { found = found + 1; } }
    }
    print(found); print("\n");
    print(tree_depth(left, right, 1)); print(tree_sum(keys, left, right, 1)); print("\n");

    set nextp: int[] = [0; 2000];
    set vals: int[] = [0; 2000];
    set head = 0;
    for (set i = 1; i < 2000; i = i + 1) { vals[i] = i * 3 % 101; nextp[i] = head; head = i; }
    set walk = 0;
    for (set r = 0; r < 60; r = r + 1) {
        set p = head;
        while (p != 0) { walk = walk + vals[p]; p = nextp[p]; }
    }
    print(walk); print("\n");
}
