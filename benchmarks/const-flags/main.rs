// const-flags: the scanner's language flags are const generic parameters.
struct Scan {
    pos: usize,
    types: u64,
    jsx: u64,
}

fn scan<const TS: bool, const JSX: bool>(s: &mut Scan, text: &[u8], depth: u32) -> u64 {
    let mut sum = 0u64;
    while s.pos < text.len() {
        let c = text[s.pos];
        s.pos += 1;
        if c == b'(' {
            if depth < 40 {
                sum += scan::<TS, JSX>(s, text, depth + 1);
            }
        } else if c == b')' && depth > 0 {
            return sum;
        } else if c == b':' && TS {
            s.types += 1;
        } else if c == b'<' && JSX {
            s.jsx += 1;
        } else if TS && c == b'?' {
            s.types += 2;
        } else {
            sum += c as u64;
        }
    }
    sum
}

fn main() {
    let mut text: Vec<u8> = Vec::new();
    let mut seed = 12345u64;
    for _ in 0..10000000 {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        let r = (seed >> 16) % 16;
        let c = match r { 0 => b'(', 1 => b')', 2 => b':', 3 => b'<', 4 => b'?', _ => b'a' };
        text.push(c);
    }
    let mut total = 0u64;
    for k in 0..100 {
        let mut s = Scan { pos: 0, types: 0, jsx: 0 };
        let found = if k % 2 == 0 { scan::<true, true>(&mut s, &text, 0) } else { scan::<false, true>(&mut s, &text, 0) };
        total += found + s.types + s.jsx;
    }
    println!("{}", total);
}
