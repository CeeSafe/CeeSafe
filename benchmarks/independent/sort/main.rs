fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn next(state: &mut u64) -> u64 {
    *state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *state >> 16
}

fn main() {
    let n = arg(1, 1000);
    let mut v: Vec<u64> = Vec::with_capacity(n as usize);
    let mut s: u64 = 7;
    for _ in 0..n {
        v.push(next(&mut s));
    }
    v.sort_unstable_by(|a, b| a.cmp(b));
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
