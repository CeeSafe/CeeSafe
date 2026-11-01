// The same bump allocator over 64 KiB chunks as main.c (and the CeeSafe runtime's Arena), with references into it: safe Rust.
use std::cell::{Cell, RefCell};

struct Node<'a> { value: u64, previous: Option<&'a Node<'a>> }

const CHUNK: usize = 65536 / std::mem::size_of::<Node>();

struct Arena<'a> { chunks: RefCell<Vec<Box<[Option<Node<'a>>]>>>, used: Cell<usize> }

impl<'a> Arena<'a> {
    fn new() -> Self { Arena { chunks: RefCell::new(Vec::new()), used: Cell::new(CHUNK) } }
    fn alloc(&'a self, node: Node<'a>) -> &'a Node<'a> {
        let mut chunks = self.chunks.borrow_mut();
        if self.used.get() == CHUNK {
            chunks.push((0..CHUNK).map(|_| None).collect());
            self.used.set(0);
        }
        let slot = &mut chunks.last_mut().unwrap()[self.used.get()];
        self.used.set(self.used.get() + 1);
        let r: *const Node<'a> = slot.insert(node);
        unsafe { &*r }   // the chunk's storage never moves while the arena lives
    }
}

fn round(r: u64) -> u64 {
    let arena = Arena::new();
    let mut last: Option<&Node> = None;
    for i in 0..200_000u64 { last = Some(arena.alloc(Node { value: (i + r) % 1000, previous: last })); }
    let mut sum = 0;
    let mut at = last;
    while let Some(n) = at { sum += n.value; at = n.previous; }
    sum
}

fn main() {
    let mut sum: u64 = 0;
    for r in 0..400 { sum += round(r); }
    println!("{}", sum);
}
