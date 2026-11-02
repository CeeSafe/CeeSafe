fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn next(state: &mut u64) -> u64 {
    *state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *state >> 16
}
struct Table {
    keys: Vec<u64>, // 0 = empty; a key is stored as key + 1
    values: Vec<u64>,
    mask: u64,
    shift: u64,
}

fn slot(t: &Table, key: u64) -> u64 {
    (key + 1).wrapping_mul(0x9E3779B97F4A7C15) >> t.shift
}

fn put(t: &mut Table, key: u64, value: u64) {
    let mut i = slot(t, key);
    loop {
        let k = t.keys[i as usize];
        if k == 0 || k == key + 1 {
            t.keys[i as usize] = key + 1;
            t.values[i as usize] = value;
            return;
        }
        i = (i + 1) & t.mask;
    }
}

fn get(t: &Table, key: u64) -> Option<u64> {
    let mut i = slot(t, key);
    loop {
        let k = t.keys[i as usize];
        if k == key + 1 {
            return Some(t.values[i as usize]);
        }
        if k == 0 {
            return None;
        }
        i = (i + 1) & t.mask;
    }
}

fn main() {
    let n = arg(1, 1000);
    let mut bits: u64 = 1;
    while (1u64 << bits) < 2 * n {
        bits += 1;
    }
    let capacity = 1u64 << bits;
    let mut t = Table { keys: Vec::with_capacity(capacity as usize), values: Vec::with_capacity(capacity as usize),
        mask: capacity - 1, shift: 64 - bits };
    for _ in 0..capacity {
        t.keys.push(0);
        t.values.push(0);
    }
    let mut s: u64 = 1;
    for i in 0..n {
        put(&mut t, next(&mut s), i);
    }
    let mut found: u64 = 0;
    let mut hits: u64 = 0;
    s = 1;
    for _ in 0..n {
        if let Some(v) = get(&t, next(&mut s)) {
            found = found.wrapping_add(v);
            hits += 1;
        }
    }
    let mut u: u64 = 99;
    for _ in 0..n {
        if get(&t, next(&mut u)).is_some() {
            hits += 1;
        }
    }
    println!("{}", found);
    println!("{}", hits);
}
