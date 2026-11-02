fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn next(state: &mut u64) -> u64 {
    *state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *state >> 16
}
fn insertion(v: &mut Vec<u64>, lo: usize, hi: usize) {
    let mut i = lo + 1;
    while i <= hi {
        let x = v[i];
        let mut j = i;
        while j > lo && v[j - 1] > x {
            v[j] = v[j - 1];
            j -= 1;
        }
        v[j] = x;
        i += 1;
    }
}

fn order(v: &mut Vec<u64>, a: usize, b: usize) {
    if v[b] < v[a] {
        v.swap(a, b);
    }
}

fn quicksort(v: &mut Vec<u64>, low: usize, high: usize) {
    let mut lo = low;
    let mut hi = high;
    while hi > lo + 16 {
        let mid = lo + (hi - lo) / 2;
        order(v, lo, mid);
        order(v, mid, hi);
        order(v, lo, mid);
        let pivot = v[mid];
        let mut i = lo;
        let mut j = hi;
        loop {
            while v[i] < pivot { i += 1; }
            while v[j] > pivot { j -= 1; }
            if i >= j { break; }
            v.swap(i, j);
            i += 1;
            j -= 1;
        }
        if j - lo < hi - j {
            quicksort(v, lo, j);
            lo = j + 1;
        } else {
            quicksort(v, j + 1, hi);
            hi = j;
        }
    }
    insertion(v, lo, hi);
}

fn main() {
    let n = arg(1, 1000);
    let mut v: Vec<u64> = Vec::with_capacity(n as usize);
    let mut s: u64 = 7;
    for _ in 0..n {
        v.push(next(&mut s));
    }
    let last = v.len() - 1;
    quicksort(&mut v, 0, last);
    let mut sorted = true;
    let mut checksum: u64 = 0;
    for i in 0..v.len() {
        if i > 0 && v[i - 1] > v[i] {
            sorted = false;
        }
        checksum = checksum.wrapping_mul(31).wrapping_add(v[i]);
    }
    println!("{}", sorted);
    println!("{}", checksum);
}
