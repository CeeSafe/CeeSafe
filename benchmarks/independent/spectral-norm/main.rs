fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn a(i: usize, j: usize) -> f64 {
    let ij = i + j;
    1.0 / ((ij * (ij + 1) / 2 + i + 1) as f64)
}

fn multiply_av(v: &[f64], out: &mut [f64]) {
    let n = v.len();
    for i in 0..n {
        let mut sum = 0.0;
        for j in 0..n {
            sum += a(i, j) * v[j];
        }
        out[i] = sum;
    }
}

fn multiply_atv(v: &[f64], out: &mut [f64]) {
    let n = v.len();
    for i in 0..n {
        let mut sum = 0.0;
        for j in 0..n {
            sum += a(j, i) * v[j];
        }
        out[i] = sum;
    }
}

fn main() {
    let n = arg(1, 100) as usize;
    let mut u = vec![1.0; n];
    let mut v = vec![0.0; n];
    let mut tmp = vec![0.0; n];
    for _ in 0..10 {
        multiply_av(&u, &mut tmp);
        multiply_atv(&tmp, &mut v);
        multiply_av(&v, &mut tmp);
        multiply_atv(&tmp, &mut u);
    }
    let mut vbv = 0.0;
    let mut vv = 0.0;
    for i in 0..n {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    println!("{}", ((vbv / vv).sqrt() * 1000000000.0) as i64);
}
