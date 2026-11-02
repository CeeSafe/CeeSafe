fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn main() {
    let limit = arg(1, 1000) as usize;
    let mut flags = vec![1u8; limit + 1];
    flags[0] = 0;
    flags[1] = 0;
    let mut i = 2usize;
    while i * i <= limit {
        if flags[i] == 1 {
            let mut j = i * i;
            while j <= limit {
                flags[j] = 0;
                j += i;
            }
        }
        i += 1;
    }
    let mut count: u64 = 0;
    let mut last = 0usize;
    for k in 0..limit + 1 {
        if flags[k] == 1 {
            count += 1;
            last = k;
        }
    }
    println!("{}", count);
    println!("{}", last);
}
