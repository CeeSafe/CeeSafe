fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn next(state: &mut u64) -> u64 {
    *state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *state >> 16
}
fn write_number(out: &mut Vec<u8>, value: u64) {
    let mut digits = [0u8; 20];
    let mut count = 0usize;
    let mut v = value;
    loop {
        digits[count] = (v % 10) as u8 + 48;
        count += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    while count > 0 {
        count -= 1;
        out.push(digits[count]);
    }
}

fn main() {
    let n = arg(1, 1000);
    let mut text: Vec<u8> = Vec::new();
    let mut s: u64 = 3;
    for i in 0..n {
        write_number(&mut text, next(&mut s) % 1000000);
        text.push(if i % 16 == 15 { b'\n' } else { b' ' });
    }
    let (mut sum, mut numbers, mut lines, mut current) = (0u64, 0u64, 0u64, 0u64);
    let mut in_number = false;
    for &b in &text {
        if b.is_ascii_digit() {
            current = current * 10 + (b - b'0') as u64;
            in_number = true;
        } else {
            if in_number {
                sum += current;
                numbers += 1;
                current = 0;
                in_number = false;
            }
            if b == b'\n' {
                lines += 1;
            }
        }
    }
    println!("{}", text.len());
    println!("{}", sum);
    println!("{}", numbers);
    println!("{}", lines);
}
