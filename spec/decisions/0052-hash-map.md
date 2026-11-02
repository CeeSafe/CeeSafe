# ADR 0052 — `HashMap` in lib/std

- Status: **Accepted** by the owner, 2026-11-02 ("Yes, I accept all five ADRs with your recommendations"): every
  question of §5 as recommended. Asked for by the owner for CeeSafe.Web
  (`~/dev/CeeSafe.Web/docs/ceesafe-upstream-work.md`, item U9): header and query maps keyed by untrusted text.
- Scope: lib/std (a key interface, a hasher, the map), one checker change (methods of `String` and `Vector`), a runtime
  source of random seeds.

## 1. Context

lib/std's collections are `Vector`, `StableVector` and `SlotMap`. The compiler has its own map (`src/support/map.cee`):
- insertion-ordered key and value vectors, and an open-addressing table of `u32` slots;
- linear probing, load 3/4, doubling;
- no removal;
- keys `K: Key<K>`, an interface with `hashCode` and `sameAs`;
- failure handled by docs/limits.md §2: a failed `put` leaves the map as it was.

A library map can start from it. Four facts decide the shape:

- **Interfaces have no `Self`.** A key interface is generic over the key, `HashKey<K>`, used as the bound
  `K: HashKey<K>`, as the compiler's `Key<K>` is. Bounds are monomorphised, so a call through one is direct
  (type-system.md §20).
- **There is no equality or hash interface in lib/core or lib/std,** and `==` exists only on integers, floats, bools,
  handles, pointers and `StringView`s.
- **`impl I for String` is accepted, but its methods cannot be called.** A method call on a `Vector` or `String`
  reaches only the built-in methods (`src/check/members.cee`'s `checkVectorMethod` reports N0200 for any other name).
  A map keyed by `String` needs that fixed.
- **Untrusted keys need a keyed hash.** With a fixed hash, a client can send header names that all collide (HashDoS).
  A hasher keyed by a per-process random seed (SipHash-1-3) avoids it. Because iteration follows insertion, not the
  hash, the seed changes no output, only timing. Programs stay deterministic, in the interpreter too.

## 2. Options

**Order after a removal.**

| | A. The last entry fills the gap (proposed) | B. Strict insertion order | C. Unordered |
|---|---|---|---|
| What | iteration is insertion order until a removal, which moves the last entry into the removed one's place, as `Vector.swapRemove` | removed entries leave tombstones, compacted when the map grows | the table's order |
| Cost | O(1) removal; the order is deterministic and simple to state | iteration skips tombstones; removal still O(1) | output depends on the seed: nondeterministic |

**A is proposed.** It is deterministic, cheap, and simple to model in an oracle.

## 3. Proposed decision

### 3.1 Keys and the hasher (lib/std, `collections.cee`)

```ceesafe
public interface HashKey<K> {
    fn hash(borrow this, h: borrow mut Hasher);
    fn sameAs(borrow this, other: borrow K) -> bool;
}

public struct Hasher {                         // SipHash-1-3, keyed
    public fn bytes(borrow mut this, data: Slice<u8>);
    public fn u64(borrow mut this, value: u64);
    public fn finish(borrow this) -> u64;
}
```

- lib/std implements `HashKey` for:
  - every integer type and `bool`;
  - `String`, by bytes;
  - `Vector<u8>`, by bytes.
- A program implements it for its own types: a struct of fields hashes each field.
- `StringView` keys are not offered: a scoped type cannot implement an interface (N0267). Use `String`, which a map
  owns.

### 3.2 The map

```ceesafe
public struct HashMap<K: HashKey<K>, V> {
    public fn create() -> HashMap<K, V>;
    public fn withSeed(k0: u64, k1: u64) -> HashMap<K, V>;           // a fixed key, for tests and for maps of trusted keys
    public fn put(borrow mut this, key: K, value: V) -> Result<V?, AllocError>;   // the old value, if the key was there
    public fn get(borrow this, key: borrow K) -> borrow V?;
    public fn getMut(borrow mut this, key: borrow K) -> borrow mut V?;
    public fn contains(borrow this, key: borrow K) -> bool;
    public fn remove(borrow mut this, key: borrow K) -> V?;
    public fn length(borrow this) -> usize;
    public fn keyAt(borrow this, i: usize) -> borrow K;              // iteration: 0..length, in the order of §2
    public fn valueAt(borrow this, i: usize) -> borrow V;
    public fn clear(borrow mut this);
}
```

- **Allocation.** Only `put` allocates. A failure leaves the map as it was (docs/limits.md §2): the table grows before
  the entry is added, and a half-added key is taken back. `remove` and `clear` never allocate.
- **The seed.** `create` uses a per-process key, read once from the system's random source:
  - `getrandom` (Linux), `arc4random_buf` (macOS, the BSDs), `BCryptGenRandom` (Windows);
  - a fixed key in the freestanding profile and in the interpreter.

  `withSeed` fixes it.
- **The allocator.** The system heap, as the compiler's map uses. A map from a user allocator follows when lib/std's
  generic structs can take an allocator parameter.

### 3.3 The checker: methods of `String` and `Vector`

A method call on a `Vector` or `String` that names no built-in method looks for an impl method before reporting N0200,
as calls on other types do. A built-in method keeps its name: an impl method with one is already N0228.

## 4. Consequences

- CeeSafe.Web keeps header and query maps in a `HashMap<String, …>`, safe from colliding keys.
- `impl` on `String` and `Vector` becomes usable, for any interface.
- New diagnostics: none.

## 5. Questions for the owner

1. `HashKey<K>` (hash into a keyed `Hasher`, and `sameAs`), with lib/std's impls for integers, `bool`, `String` and
   `Vector<u8>`? Recommended: yes.
2. Iteration in insertion order, a removal moving the last entry into the gap (option A)? Recommended: yes.
3. SipHash-1-3 keyed per process from the system's random source, with `withSeed` to fix it? Recommended: yes.
4. Method calls on `String` and `Vector` reach impl methods (§3.3)? Recommended: yes.
5. The system heap only for now, a user allocator later? Recommended: yes.

## 6. Implementation plan (once accepted)

1. Spec: a collections section beside §23–24, and §21 for `HashKey`; the guide's collections page.
2. lib/std: the interface, the impls, the hasher, the map; `tools/gen_library.py`.
3. Runtime: `ceesafe_random_key` for the seed; `tools/gen_runtime.py`.
4. The checker change of §3.3, and the interpreter's model of the seed (a fixed key).
5. Tests, with the support library's approach:
   - a Python oracle drives random `put`, `get`, `remove` and iteration against a `dict` whose order is kept as §2
     says (`tools/oracles/`);
   - SipHash-1-3 against the reference vectors;
   - the allocation-failure sweep with `--check-leaks`, each failed `put` leaving the map as it was;
   - many keys that collide under a fixed hash, which a keyed one spreads;
   - check-fail: a key type without `HashKey` (N0268).
6. ROADMAP §10.
