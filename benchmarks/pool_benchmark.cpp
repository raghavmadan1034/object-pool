// Benchmarks: objpool::ObjectPool vs. the general-purpose heap (new/delete).
//
// The point of these numbers is NOT that the pool has a lower mean. A modern
// allocator's thread-local fast path is already very good at a tight
// new/delete loop. The point is that the pool:
//
//   1. collapses the *distribution* - no size-class lookup, no arena lock,
//      no chance of falling off the fast path into the OS,
//   2. keeps objects inside one contiguous slab, so iterating over live
//      objects is a linear scan the hardware prefetcher can follow,
//   3. does not degrade when the free/alloc order stops being LIFO.
//
// Run with repetitions so the stddev/CV columns are meaningful:
//   pool_benchmark --benchmark_repetitions=10 --benchmark_report_aggregates_only=true

#include <benchmark/benchmark.h>

#include "object_pool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace {

// 32 bytes: about the size of a market-data tick or a small game entity.
// Half a cache line, so exactly two fit per 64-byte line.
struct Particle {
    float x, y, z;
    float vx, vy, vz;
    std::uint32_t id;
    std::uint32_t flags;

    Particle(float x_, float y_) noexcept
        : x(x_), y(y_), z(0.0f), vx(0.0f), vy(0.0f), vz(0.0f), id(0), flags(0) {}
};

static_assert(sizeof(Particle) == 32, "benchmark assumes a 32-byte payload");

constexpr std::size_t kCapacity = 4096;              // 4096 * 32 B = 128 KiB slab
using Pool = objpool::ObjectPool<Particle, kCapacity>;

// 128 KiB is far too large for a stack frame (Windows defaults to 1 MiB of
// stack for the whole thread), so the pool itself lives on the heap. That is
// one allocation for the entire program, not one per object - which is the
// distinction the pool actually makes.
std::unique_ptr<Pool> make_pool() { return std::make_unique<Pool>(); }

// ---------------------------------------------------------------------------
// 1. Single acquire + release. The allocator's best case.
// ---------------------------------------------------------------------------

void BM_Pool_AcquireRelease(benchmark::State& state) {
    auto pool = make_pool();
    for (auto _ : state) {
        Particle* p = pool->acquire(1.0f, 2.0f);
        benchmark::DoNotOptimize(p);
        pool->release(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Pool_AcquireRelease);

void BM_Heap_NewDelete(benchmark::State& state) {
    for (auto _ : state) {
        Particle* p = new Particle(1.0f, 2.0f);
        benchmark::DoNotOptimize(p);
        delete p;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Heap_NewDelete);

// ---------------------------------------------------------------------------
// 2. Batch churn: acquire N, then release all N. This is the realistic shape -
//    a frame of a simulation, or a burst of messages drained at end of tick.
// ---------------------------------------------------------------------------

void BM_Pool_BatchChurn(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto pool = make_pool();
    std::vector<Particle*> live(n);

    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            live[i] = pool->acquire(static_cast<float>(i), 2.0f);
        }
        benchmark::DoNotOptimize(live.data());
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < n; ++i) {
            pool->release(live[i]);
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Pool_BatchChurn)->Arg(64)->Arg(512)->Arg(4096);

void BM_Heap_BatchChurn(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<Particle*> live(n);

    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            live[i] = new Particle(static_cast<float>(i), 2.0f);
        }
        benchmark::DoNotOptimize(live.data());
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < n; ++i) {
            delete live[i];
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Heap_BatchChurn)->Arg(64)->Arg(512)->Arg(4096);

// ---------------------------------------------------------------------------
// 3. Shuffled release. Freeing in an order unrelated to allocation order is
//    what real programs do, and it is where a size-class allocator's free
//    lists stop being tidy. The pool is a single linked push - it cannot care.
// ---------------------------------------------------------------------------

std::vector<std::size_t> shuffled_indices(std::size_t n) {
    std::vector<std::size_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = i;
    std::mt19937 rng(12345);                  // fixed seed: reproducible runs
    std::shuffle(idx.begin(), idx.end(), rng);
    return idx;
}

void BM_Pool_ShuffledRelease(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto pool = make_pool();
    std::vector<Particle*> live(n);
    const std::vector<std::size_t> order = shuffled_indices(n);

    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            live[i] = pool->acquire(static_cast<float>(i), 2.0f);
        }
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < n; ++i) {
            pool->release(live[order[i]]);
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Pool_ShuffledRelease)->Arg(512)->Arg(4096);

void BM_Heap_ShuffledDelete(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<Particle*> live(n);
    const std::vector<std::size_t> order = shuffled_indices(n);

    for (auto _ : state) {
        for (std::size_t i = 0; i < n; ++i) {
            live[i] = new Particle(static_cast<float>(i), 2.0f);
        }
        benchmark::ClobberMemory();
        for (std::size_t i = 0; i < n; ++i) {
            delete live[order[i]];
        }
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Heap_ShuffledDelete)->Arg(512)->Arg(4096);

// ---------------------------------------------------------------------------
// 4. Traversal locality. Allocation cost is paid once; iteration cost is paid
//    every frame. Pool objects sit in one 128 KiB slab. Heap objects are
//    interleaved with unrelated allocations, exactly as they would be in a
//    real program, so they land on scattered cache lines.
// ---------------------------------------------------------------------------

void BM_Pool_Traverse(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto pool = make_pool();
    std::vector<Particle*> live(n);
    for (std::size_t i = 0; i < n; ++i) {
        live[i] = pool->acquire(static_cast<float>(i), 2.0f);
    }

    for (auto _ : state) {
        float sum = 0.0f;
        for (std::size_t i = 0; i < n; ++i) sum += live[i]->x;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * n);

    for (std::size_t i = 0; i < n; ++i) pool->release(live[i]);
}
BENCHMARK(BM_Pool_Traverse)->Arg(512)->Arg(4096);

void BM_Heap_Traverse(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    std::vector<Particle*> live(n);
    std::vector<std::unique_ptr<std::uint64_t>> noise(n);
    for (std::size_t i = 0; i < n; ++i) {
        live[i]  = new Particle(static_cast<float>(i), 2.0f);
        noise[i] = std::make_unique<std::uint64_t>(i);   // fragment the heap
    }

    for (auto _ : state) {
        float sum = 0.0f;
        for (std::size_t i = 0; i < n; ++i) sum += live[i]->x;
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * n);

    for (std::size_t i = 0; i < n; ++i) delete live[i];
}
BENCHMARK(BM_Heap_Traverse)->Arg(512)->Arg(4096);

}  // namespace

BENCHMARK_MAIN();
