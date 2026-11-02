fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn main() {
    let n = arg(1, 100) as usize;
    let mut a = Vec::with_capacity(n * n);
    let mut b = Vec::with_capacity(n * n);
    for i in 0..n {
        for j in 0..n {
            a.push(((i * j) % 7) as f64);
            b.push(((i + 2 * j) % 5) as f64);
        }
    }
    let mut c = vec![0.0f64; n * n];
    for i in 0..n {
        for k in 0..n {
            let aik = a[i * n + k];
            for j in 0..n {
                c[i * n + j] += aik * b[k * n + j];
            }
        }
    }
    let mut sum = 0.0;
    let mut trace = 0.0;
    for i in 0..n {
        trace += c[i * n + i];
        for j in 0..n {
            sum += c[i * n + j];
        }
    }
    println!("{}", sum as i64);
    println!("{}", trace as i64);
}
