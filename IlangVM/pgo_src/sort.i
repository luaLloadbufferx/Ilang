// PGO training: sorting and searching over int[] (bounds-checked indexing), swaps, comparisons, binary search.
set rng = 12345;

fn rnd() -> int {
    rng = (rng * 1103515245 + 12345) % 2147483647;
    if (rng < 0) { rng = 0 - rng; }
    return rng;
}

fn fill(a: int[], n: int) {
    for (set i = 0; i < n; i = i + 1) { a[i] = rnd() % 100000; }
}

fn insertion_sort(a: int[], n: int) {
    for (set i = 1; i < n; i = i + 1) {
        set key = a[i];
        set j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j = j - 1;
        }
        a[j + 1] = key;
    }
}

fn bubble_sort(a: int[], n: int) {
    for (set i = 0; i < n; i = i + 1) {
        set swapped = false;
        for (set j = 0; j < n - 1 - i; j = j + 1) {
            if (a[j] > a[j + 1]) {
                set t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
                swapped = true;
            }
        }
        if (!swapped) { break; }
    }
}

fn quick_sort(a: int[], lo: int, hi: int) {
    // recurse into the smaller part and loop on the larger one: the depth stays below log2(n) (the data stack is small)
    while (lo < hi) {
        set p = a[(lo + hi) / 2];
        set i = lo;
        set j = hi;
        while (i <= j) {
            while (a[i] < p) { i = i + 1; }
            while (a[j] > p) { j = j - 1; }
            if (i <= j) {
                set t = a[i]; a[i] = a[j]; a[j] = t;
                i = i + 1;
                j = j - 1;
            }
        }
        if (j - lo < hi - i) {
            quick_sort(a, lo, j);
            lo = i;
        } else {
            quick_sort(a, i, hi);
            hi = j;
        }
    }
}

fn sift_down(a: int[], start: int, end: int) {
    set root = start;
    while (root * 2 + 1 <= end) {
        set child = root * 2 + 1;
        set sw = root;
        if (a[sw] < a[child]) { sw = child; }
        if (child + 1 <= end && a[sw] < a[child + 1]) { sw = child + 1; }
        if (sw == root) { return; }
        set t = a[root]; a[root] = a[sw]; a[sw] = t;
        root = sw;
    }
}

fn heap_sort(a: int[], n: int) {
    set start = (n - 2) / 2;
    while (start >= 0) { sift_down(a, start, n - 1); start = start - 1; }
    set end = n - 1;
    while (end > 0) {
        set t = a[0]; a[0] = a[end]; a[end] = t;
        end = end - 1;
        sift_down(a, 0, end);
    }
}

fn is_sorted(a: int[], n: int) -> bool {
    for (set i = 1; i < n; i = i + 1) { if (a[i - 1] > a[i]) { return false; } }
    return true;
}

fn bsearch(a: int[], n: int, v: int) -> int {
    set lo = 0;
    set hi = n - 1;
    while (lo <= hi) {
        set mid = (lo + hi) / 2;
        if (a[mid] == v) { return mid; }
        if (a[mid] < v) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return -1;
}

fn checksum(a: int[], n: int) -> int {
    set s = 0;
    for (set i = 0; i < n; i = i + 1) { s = (s * 31 + a[i]) % 1000003; }
    return s;
}

fn main() {
    set n = 500;
    set a: int[] = [0; n];

    fill(a, n); insertion_sort(a, n);
    print(is_sorted(a, n)); print(checksum(a, n)); print("\n");

    fill(a, n); bubble_sort(a, n);
    print(is_sorted(a, n)); print(checksum(a, n)); print("\n");

    set big = 12000;
    set b: int[] = [0; big];
    fill(b, big); quick_sort(b, 0, big - 1);
    print(is_sorted(b, big)); print(checksum(b, big)); print("\n");

    fill(b, big); heap_sort(b, big);
    print(is_sorted(b, big)); print(checksum(b, big)); print("\n");

    set found = 0;
    for (set i = 0; i < 20000; i = i + 1) {
        if (bsearch(b, big, rnd() % 100000) >= 0) { found = found + 1; }
    }
    print(found); print("\n");

    // already sorted / reverse sorted input
    for (set i = 0; i < n; i = i + 1) { a[i] = n - i; }
    insertion_sort(a, n);
    print(is_sorted(a, n)); print("\n");
}
