fn arg(i: usize, fallback: u64) -> u64 {
    std::env::args().nth(i).map(|s| s.parse().unwrap()).unwrap_or(fallback)
}
fn next(state: &mut u64) -> u64 {
    *state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *state >> 16
}
trait Shape {
    fn area(&self) -> f64;
    fn scale(&mut self, by: f64);
}

struct Circle { r: f64 }
impl Shape for Circle {
    fn area(&self) -> f64 { 3.141592653589793 * self.r * self.r }
    fn scale(&mut self, by: f64) { self.r *= by; }
}

struct Rect { w: f64, h: f64 }
impl Shape for Rect {
    fn area(&self) -> f64 { self.w * self.h }
    fn scale(&mut self, by: f64) { self.w *= by; }
}

struct Triangle { b: f64, h: f64 }
impl Shape for Triangle {
    fn area(&self) -> f64 { 0.5 * self.b * self.h }
    fn scale(&mut self, by: f64) { self.h *= by; }
}

fn main() {
    let rounds = arg(1, 10);
    let mut shapes: Vec<Box<dyn Shape>> = Vec::new();
    let mut s: u64 = 5;
    for _ in 0..1000 {
        let x = (next(&mut s) % 100) as f64 / 10.0 + 0.5;
        let kind = next(&mut s) % 3;
        if kind == 0 {
            shapes.push(Box::new(Circle { r: x }));
        } else if kind == 1 {
            shapes.push(Box::new(Rect { w: x, h: x + 1.0 }));
        } else {
            shapes.push(Box::new(Triangle { b: x, h: 2.0 * x }));
        }
    }
    let mut total = 0.0;
    for r in 0..rounds {
        for shape in &shapes {
            total += shape.area();
        }
        let k = (r % 1000) as usize;
        shapes[k].scale(1.0000001);
    }
    println!("{}", total as i64);
}
