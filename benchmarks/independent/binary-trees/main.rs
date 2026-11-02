fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
struct Node {
    left: Option<Box<Node>>,
    right: Option<Box<Node>>,
}

fn make(depth: u32) -> Box<Node> {
    if depth == 0 {
        return Box::new(Node { left: None, right: None });
    }
    Box::new(Node { left: Some(make(depth - 1)), right: Some(make(depth - 1)) })
}

fn check(node: &Node) -> u64 {
    let mut total: u64 = 1;
    if let Some(l) = &node.left {
        total += check(l);
    }
    if let Some(r) = &node.right {
        total += check(r);
    }
    total
}

fn main() {
    let max_depth = arg(1, 10) as u32;
    {
        let stretch = make(max_depth + 1);
        println!("{}", check(&stretch));
    }
    let long_lived = make(max_depth);
    let mut depth: u32 = 4;
    let mut total: u64 = 0;
    while depth <= max_depth {
        let iterations: u64 = 1u64 << (max_depth - depth + 4);
        let mut sum: u64 = 0;
        for _ in 0..iterations {
            let t = make(depth);
            sum += check(&t);
        }
        total += sum;
        depth += 2;
    }
    println!("{}", total);
    println!("{}", check(&long_lived));
}
