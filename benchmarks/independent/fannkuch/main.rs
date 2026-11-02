fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn main() {
    let n = arg(1, 7) as usize;
    let mut perm = vec![0i32; n];
    let mut perm1: Vec<i32> = (0..n as i32).collect();
    let mut count = vec![0i32; n];
    let mut max_flips = 0;
    let mut checksum = 0;
    let mut perm_count = 0;
    let mut r = n;
    loop {
        while r != 1 {
            count[r - 1] = r as i32;
            r -= 1;
        }
        for i in 0..n {
            perm[i] = perm1[i];
        }
        let mut flips = 0;
        let mut k = perm[0];
        while k != 0 {
            let mut lo = 0usize;
            let mut hi = k as usize;
            while lo < hi {
                perm.swap(lo, hi);
                lo += 1;
                hi -= 1;
            }
            flips += 1;
            k = perm[0];
        }
        if flips > max_flips {
            max_flips = flips;
        }
        if perm_count % 2 == 0 {
            checksum += flips;
        } else {
            checksum -= flips;
        }
        loop {
            if r == n {
                println!("{}", checksum);
                println!("{}", max_flips);
                return;
            }
            let perm0 = perm1[0];
            for i in 0..r {
                perm1[i] = perm1[i + 1];
            }
            perm1[r] = perm0;
            count[r] -= 1;
            if count[r] > 0 {
                break;
            }
            r += 1;
        }
        perm_count += 1;
    }
}
