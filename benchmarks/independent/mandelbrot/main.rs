fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn main() {
    let n = arg(1, 200) as u32;
    let size = n as f64;
    let mut checksum: u64 = 0;
    for y in 0..n {
        let ci = 2.0 * (y as f64) / size - 1.0;
        let mut bits: u32 = 0;
        let mut bit: u32 = 0;
        for x in 0..n {
            let cr = 2.0 * (x as f64) / size - 1.5;
            let (mut zr, mut zi, mut tr, mut ti) = (0.0f64, 0.0f64, 0.0f64, 0.0f64);
            let mut i = 0;
            while i < 50 && tr + ti <= 4.0 {
                zi = 2.0 * zr * zi + ci;
                zr = tr - ti + cr;
                tr = zr * zr;
                ti = zi * zi;
                i += 1;
            }
            bits <<= 1;
            if tr + ti <= 4.0 {
                bits |= 1;
            }
            bit += 1;
            if bit == 8 || x == n - 1 {
                bits <<= 8 - bit;
                checksum = checksum.wrapping_mul(31).wrapping_add(bits as u64);
                bits = 0;
                bit = 0;
            }
        }
    }
    println!("{}", checksum);
}
