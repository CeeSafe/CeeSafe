fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn next(state: &mut u64) -> u64 {
    *state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *state >> 16
}

use std::collections::HashMap;

fn main() {
    let n = arg(1, 1000);
    let mut map: HashMap<u64, u64> = HashMap::new();
    let mut s: u64 = 1;
    for i in 0..n {
        let k = next(&mut s);
        map.insert(k, i);
    }
    let mut found: u64 = 0;
    let mut hits: u64 = 0;
    s = 1;
    for _ in 0..n {
        let k = next(&mut s);
        if let Some(v) = map.get(&k) {
            found = found.wrapping_add(*v);
            hits += 1;
        }
    }
    let mut t: u64 = 99;
    for _ in 0..n {
        let k = next(&mut t);
        if map.get(&k).is_some() {
            hits += 1;
        }
    }
    s = 1;
    let mut removed: u64 = 0;
    for _ in 0..(n / 2) {
        let k = next(&mut s);
        if map.remove(&k).is_some() {
            removed += 1;
        }
    }
    println!("{}", found);
    println!("{}", hits);
    println!("{}", removed);
    println!("{}", map.len());
}
