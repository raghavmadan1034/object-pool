# object-pool

A fixed-capacity, header-only object pool for C++17, with a move-only RAII handle.

Allocating and freeing many short-lived objects of the same type is a workload that
general-purpose allocators are not built for. `new` has to pick a size class, consult a
free list, possibly take a lock, and hand back memory that may be anywhere in the address
space. If you know the type and an upper bound on how many are alive at once, you can
replace all of that with a pointer dereference.

```cpp
#include "object_pool.hpp"

objpool::ObjectPool<Particle, 1024> pool;     // one contiguous slab, no heap traffic

Particle* p = pool.acquire(1.0f, 2.0f);       // O(1): pop the free list, placement-new
pool.release(p);                              // O(1): ~Particle(), push the free list
```

Measured on the machine described below: **0.98 ns** per acquire+release against **20.4 ns**
for `new`/`delete` — about **20x**.

---

## Table of contents

- [Design](#design)
- [API](#api)
- [RAII handle](#raii-handle)
- [Benchmarks](#benchmarks)
- [Choosing where the pool lives](#choosing-where-the-pool-lives)
- [When *not* to use this](#when-not-to-use-this)
- [Building](#building)
- [Status](#status)

---

## Design

### Capacity is a template parameter, not a constructor argument

```cpp
template <typename T, std::size_t Capacity, typename ExhaustionPolicy = ThrowOnExhaustion>
class ObjectPool;
```

This is the decision everything else follows from. Because `Capacity` is known at compile
time, the slab is an ordinary member array rather than a heap allocation:

- **No allocation at all.** Not "one allocation instead of N" — zero. A `static` or
  `thread_local` pool touches the allocator never.
- **Construction is `noexcept`.** There is nothing that can fail.
- **There is no moved-from state.** A pool cannot be moved (its objects' addresses are
  baked into the slab), so the Rule of Five collapses to four `= delete` lines and there
  is no "empty but alive" state to reason about, test, or get wrong.

The cost is that `sizeof(ObjectPool<T, N>)` is the entire slab, which matters for where
you can put one — see [Choosing where the pool lives](#choosing-where-the-pool-lives).

### The free list lives inside the free slots

A slot is either holding a live `T` or sitting on the free list. It is never both. So the
"next free slot" pointer is stored *in the slot's own memory*:

```cpp
struct Node { Node* next; };
```

The result is **zero per-slot metadata**. A separate `next[]` array or a `bitset` would
cost memory and a second cache miss on every operation; here the pointer you need is in
the line you are already touching.

Three distinct problems have to be solved to make one buffer serve both roles:

| Problem | Fix |
|---|---|
| `T` may be smaller than `Node` | `RawSlotSize = max(sizeof(T), sizeof(Node))` |
| `T` may be less strictly aligned than `Node` | `SlotAlign = max(alignof(T), alignof(Node))` |
| that size may not be a multiple of that alignment | round up: `((x + a - 1) / a) * a` |

`max` is correct rather than `lcm` because alignments are always powers of two, so the
larger always divides into the smaller's multiples.

### The free list is LIFO, deliberately

`release()` pushes to the front and `acquire()` pops from the front, so the slot you get
back is the one you *just* returned — still hot in L1. A FIFO queue would hand you the
coldest slot every time, turning a ~1 ns access into a ~80-100 ns DRAM round trip. The
benchmarks below measure this effect directly.

### `acquire()` gives the strong exception guarantee

The subtle bug: `placement new` constructs `T` **on top of** the bytes that hold the
free-list link. If `T`'s constructor then throws, the link is already gone and the pool is
corrupt. The fix is to cache the link *before* constructing, and rebuild the `Node` if
construction fails:

```cpp
Node* next = slot->next;                 // read BEFORE the bytes are overwritten
try {
    T* obj = new (slot) T(std::forward<Args>(args)...);
    free_head_ = next;                   // commit only on success
    ++in_use_;
    return obj;
} catch (...) {
    ::new (slot) Node{next};             // put the slot back exactly as it was
    throw;
}
```

Note that the catch block does **not** call `~T()`. If a constructor throws, the object was
never constructed, and destroying it would be undefined behaviour.

### Exhaustion is a compile-time policy

```cpp
struct ThrowOnExhaustion { static T* on_exhausted() { throw pool_exhausted{}; } };
struct NullOnExhaustion  { static T* on_exhausted() noexcept { return nullptr; } };
struct AbortOnExhaustion { static T* on_exhausted() noexcept { std::abort(); } };
```

A policy *type* can carry arbitrary code; an `enum` parameter could only select from a
fixed menu, and would cost a runtime branch. This costs **zero bytes** of object size and
zero branches, and `acquire()`'s `noexcept` specification is computed from it:

```cpp
noexcept(std::is_nothrow_constructible_v<T, Args...> && noexcept(ExhaustionPolicy::on_exhausted()))
```

so `ObjectPool<T, N, NullOnExhaustion>` is genuinely `noexcept` for a `noexcept`-constructible
`T`, and the throwing configuration is correctly not.

`pool_exhausted` derives from `std::bad_alloc` — it is an allocation failure, and code that
already catches `std::bad_alloc` should catch this too.

### Debug-only integrity checks

`release()` asserts that the pointer actually belongs to this pool and is not already free:

- **`owns(p)`** — O(1). Range check plus an alignment check, done in `std::uintptr_t`
  rather than pointer arithmetic, because relational comparison of pointers into
  *different* objects is unspecified, and testing that premise with an operation only
  defined when it holds is circular.
- **`is_in_free_list(p)`** — O(N) walk that catches double-release.

Both compile to nothing under `NDEBUG`. Because they are members of a class template,
they are not merely dead-stripped — they are never instantiated. The O(N) check is exactly
the tradeoff `_GLIBCXX_DEBUG` and MSVC's `_ITERATOR_DEBUG_LEVEL=2` make: a debug build that
finds the bug, and a release build that is untouched.

`owns()` proves a pointer is *plausible*, not that it is *live*. If `SlotSize == sizeof(T)`,
then `obj + 1` passes the range and alignment tests. It is a guard rail, not a proof.

---

## API

```cpp
namespace objpool {

template <typename T, std::size_t Capacity, typename ExhaustionPolicy = ThrowOnExhaustion>
class ObjectPool {
public:
    ObjectPool() noexcept;
    ~ObjectPool();                                    // does NOT destroy live objects

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&)                 = delete;
    ObjectPool& operator=(ObjectPool&&)      = delete;

    template <typename... Args>
    [[nodiscard]] T* acquire(Args&&... args) noexcept(/* see above */);
    void release(T* obj) noexcept;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
    [[nodiscard]] std::size_t size()            const noexcept;   // objects in use
    [[nodiscard]] std::size_t available()       const noexcept;
    [[nodiscard]] bool        exhausted()       const noexcept;
    [[nodiscard]] std::size_t high_water_mark() const noexcept;   // peak ever in use
    [[nodiscard]] PoolStats   stats()           const noexcept;

    [[nodiscard]] bool owns(const T* p) const noexcept;
};

}  // namespace objpool
```

`capacity()` is `static constexpr` because capacity is a property of the *type*, not of any
instance — `ObjectPool<Particle, 8>::capacity()` is usable in a constant expression. (It is
therefore not `const`: a static member function has no `this` to qualify.)

`high_water_mark()` is the number worth logging. It tells you whether `Capacity` was sized
correctly, which is the one question a fixed-capacity pool forces you to answer.

There is no `empty()` or `full()`. `exhausted()` is derived from the free-list head, so it
cannot disagree with what `acquire()` will actually do.

---

## RAII handle

Raw `acquire()`/`release()` has the same problem as raw `new`/`delete`: every `return`,
`break`, and `throw` between them is a chance to leak a slot. A leaked slot is worse than
a leaked heap block, because the pool is finite — leak enough and `acquire()` starts
failing in production.

```cpp
auto h = pool.make(1.0f, 2.0f);   // acquire + wrap, one step
h->x += 1.0f;                     // use it like a pointer
                                  // slot returns to the pool at scope exit, on every path
```

`PoolHandle<T, Pool>` is `std::unique_ptr` with a pool-returning deleter instead of
`delete`: two pointers wide, **no control block, no reference count, no atomics**.
Ownership is unique and transferable, never shared:

```cpp
auto h2 = std::move(h1);          // ownership moves; h1 is now empty
auto h3 = h1;                     // compile error: copies are deleted

std::vector<PoolHandle<Particle, Pool>> v;
v.push_back(pool.make(0.f, 0.f)); // moved in
v.clear();                        // every slot returns to the pool
```

**Why not shared ownership?** `shared_ptr` would heap-allocate a ~32-byte control block per
object — the exact allocation the pool exists to eliminate, and `make_shared` cannot fuse it
away because the object lives in the slab. It would also add atomic refcounting (cheap
uncontended, 40-100 ns when the count ping-pongs between cores) and a type-erased, therefore
virtual, destroy call. The strongest reason is neither of those: shared ownership makes
*release time* nondeterministic, which destroys the LIFO locality that makes reuse fast. A
caller who genuinely needs sharing can wrap a unique handle in a `shared_ptr` in one line.
Unique composes upward; shared cannot compose downward.

> **Lifetime rule:** a handle must never outlive its pool. Declare the pool first, so
> reverse destruction order tears handles down before the slab.

---

## Benchmarks

Google Benchmark, GCC 16.1 `-O3 -DNDEBUG`, 32-core x86-64 @ 2994 MHz
(L1d 48 KiB x16, L2 1 MiB x16, L3 32 MiB x2), 15 repetitions, process pinned to one core
at high priority. Payload is a 32-byte `Particle`. Figures are medians.

```
pool_benchmark --benchmark_repetitions=15 --benchmark_report_aggregates_only=true
```

| Workload | Pool | `new`/`delete` | Speedup |
|---|---:|---:|---:|
| Single acquire + release | **0.98 ns** | 20.4 ns | **20.9x** |
| Batch churn, 64 objects | **86 ns** | 1890 ns | **22.0x** |
| Batch churn, 512 objects | **723 ns** | 13.4 µs | **18.5x** |
| Batch churn, 4096 objects | **5.88 µs** | 97.2 µs | **16.5x** |
| Shuffled release, 512 | **769 ns** | 13.4 µs | **17.4x** |
| Shuffled release, 4096 | **11.9 µs** | 97.4 µs | **8.2x** |
| Traverse 512 live objects | 188 ns | 183 ns | **0.97x** |
| Traverse 4096 live objects | 1638 ns | 1644 ns | **1.00x** |

### Reading these honestly

**This benchmark does not measure tail latency, and no claim here depends on it.**
Google Benchmark reports mean, median and standard deviation *across repetitions*, and
each repetition is itself an average over millions of iterations. That averaging hides the
per-operation outliers a latency-sensitive caller actually cares about. The coefficient of
variation came out similar for both allocators — 12.4% pool vs 9.8% heap when pinned,
55.5% vs 53.7% unpinned — which says the *machine* was noisy, not that either allocator
has a tighter distribution. The structural case for the pool's determinism (no size-class
lookup, no arena lock, nothing that can fall through to the OS) is sound, but it is an
argument, not a measurement. Earning a p99/p99.9 number requires per-operation timestamps
and a histogram, which this suite does not yet collect.

**Shuffled release costs the pool 2x, and this is the LIFO effect made visible.**
Per-operation, batch churn is 1.44 ns and shuffled release is 2.91 ns. Releasing in an
order unrelated to allocation order means each push writes to a scattered slot instead of
a hot one. `malloc` was essentially unaffected (23.7 vs 23.8 ns/op), so the pool's
*relative* advantage narrows from 16.5x to 8.2x. The pool's speed is not free — it is
bought with locality, and this row is the price tag.

**Traversal is a dead heat, and that row stays in the table.** At 4096 objects the working
set is 128 KiB, which fits comfortably in a 1 MiB L2 whether the objects are contiguous or
scattered, and the hardware prefetcher handles both. The contiguity argument for pools is
real, but it needs a working set that actually exceeds cache to show up, and this benchmark
does not create one. Reporting it is more useful than deleting it.

**This is a microbenchmark.** It measures allocator throughput in a tight loop on one
thread with a warm cache. It does not measure a real program.

---

## Choosing where the pool lives

`sizeof(ObjectPool<T, N>)` is the whole slab. `ObjectPool<Particle, 4096>` is 128 KiB, and
the default thread stack is 1 MiB on Windows and 8 MiB on Linux.

| Where | When | Note |
|---|---|---|
| Local variable | slab well under ~100 KiB | Fastest. Zero setup. |
| `static` | one pool for the process | No allocation ever; not thread-safe |
| `thread_local` | per-thread pools | **The right answer for multithreading** |
| `std::make_unique<Pool>()` | large slab, dynamic lifetime | One allocation total, not one per object |

Rule of thumb: past ~100 KiB, get it off the stack.

---

## When *not* to use this

| Situation | Why the pool is the wrong tool |
|---|---|
| Objects of varying type or size | One pool serves exactly one `T`. Use the heap or an arena. |
| Unbounded or unpredictable object count | Fixed capacity means you must pick a bound. Get it wrong upward and you waste memory; downward and you fail at peak load. |
| Long-lived objects | The win is in high-frequency churn. Allocating once and living forever gains nothing. |
| Shared across threads | Not synchronised. Use one pool per thread (`thread_local`). |
| Objects that outlive the pool | Every handle must die before the slab does. |
| You have not profiled | If the allocator is not on your profile, this is complexity for nothing. |

This pool is **not thread-safe**, and that is a design decision rather than an omission.
Guarding it with a mutex would reintroduce exactly the cross-core contention it exists to
avoid; the correct scaling story is one pool per thread.

---

## Building

Header-only. Copy `include/object_pool.hpp` into your project and include it — that is the
whole integration story. C++17, no external dependencies, no C++20 required.

For the tests and benchmarks:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOBJECTPOOL_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/pool_benchmark --benchmark_repetitions=15 --benchmark_report_aggregates_only=true
```

Benchmarks are **off by default** because they fetch Google Benchmark over the network.
As a CMake target:

```cmake
add_subdirectory(object-pool)
target_link_libraries(my_app PRIVATE objectpool::objectpool)
```

Verified on GCC 16.1 (`-Wall -Wextra -Wpedantic`, zero warnings) and MSVC 19.4x
(`/W4 /permissive-`, zero warnings), including under AddressSanitizer.

> One caveat on sanitizers: ASan instruments `malloc`/`free`, so it cannot see a
> use-after-free *inside a pool slot* — to ASan the slab is one long-lived live allocation.
> The debug asserts exist partly to cover that gap. Poisoning released slots with
> `__asan_poison_memory_region` would close it and is the natural next step.

---

## Status

The library is feature-complete: `include/object_pool.hpp` and `include/pool_handle.hpp` are
both done, and `ObjectPool::make()` ties them together.

Remaining work is around the library rather than in it. `tests/smoke.cpp` currently covers only
the raw `acquire`/`release` API — the handle's move semantics, `reset()`, `detach()` and `swap()`
have no coverage yet, and that is the next gap to close. After that: worked examples under
`examples/`, and converting the test suite to Catch2.
