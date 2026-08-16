#include "object_pool.hpp"

#include <cstdio>
#include <string>
#include <stdexcept>
#include <type_traits>

using namespace objpool;

struct Particle {
    float x, y, vx, vy;
    Particle(float a, float b) : x(a), y(b), vx(0), vy(0) {}
};

struct Tiny { char c; };                       // smaller than Node -> padding path

struct Odd { char d[9]; };                     // size not a multiple of align

struct Boom {
    int v;
    explicit Boom(bool blow) : v(1) { if (blow) throw std::runtime_error("boom"); }
};

struct Loud {                                  // non-trivial destructor
    static int destroyed;
    ~Loud() { ++destroyed; }
};
int Loud::destroyed = 0;

int failures = 0;

#define CHECK(cond)                                                        \
    do { if (!(cond)) { std::printf("FAIL %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main() {
    // --- basic acquire / release ---------------------------------------
    {
        ObjectPool<Particle, 4> pool;
        CHECK(pool.capacity() == 4);
        CHECK(pool.size() == 0);
        CHECK(pool.available() == 4);
        CHECK(!pool.exhausted());

        Particle* p = pool.acquire(1.0f, 2.0f);
        CHECK(p != nullptr);
        CHECK(p->x == 1.0f && p->y == 2.0f);
        CHECK(pool.size() == 1);
        CHECK(pool.available() == 3);
        CHECK(pool.owns(p));
        CHECK(pool.high_water_mark() == 1);

        pool.release(p);
        CHECK(pool.size() == 0);
        CHECK(pool.high_water_mark() == 1);      // sticky
    }

    // --- LIFO reuse: same slot comes back -------------------------------
    {
        ObjectPool<Particle, 4> pool;
        Particle* a = pool.acquire(0.f, 0.f);
        pool.release(a);
        Particle* b = pool.acquire(9.f, 9.f);
        CHECK(a == b);
        pool.release(b);
    }

    // --- exhaustion: throw policy (default) -----------------------------
    {
        ObjectPool<Particle, 2> pool;
        Particle* a = pool.acquire(0.f, 0.f);
        Particle* b = pool.acquire(0.f, 0.f);
        CHECK(pool.exhausted());
        bool threw = false;
        try { (void)pool.acquire(0.f, 0.f); }
        catch (const pool_exhausted&)  { threw = true; }
        catch (const std::bad_alloc&)  { threw = true; }
        CHECK(threw);
        CHECK(pool.size() == 2);                 // unchanged after the throw
        pool.release(a);
        pool.release(b);
    }

    // --- exhaustion: null policy ----------------------------------------
    {
        ObjectPool<Particle, 1, NullOnExhaustion> pool;
        Particle* a = pool.acquire(0.f, 0.f);
        CHECK(a != nullptr);
        Particle* b = pool.acquire(0.f, 0.f);
        CHECK(b == nullptr);
        pool.release(a);
    }

    // --- strong exception guarantee -------------------------------------
    {
        ObjectPool<Boom, 4> pool;
        Boom* ok = pool.acquire(false);
        CHECK(ok != nullptr);
        CHECK(pool.size() == 1);

        bool threw = false;
        try { (void)pool.acquire(true); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
        CHECK(pool.size() == 1);                 // stats NOT corrupted
        CHECK(pool.available() == 3);

        Boom* again = pool.acquire(false);       // free list still intact
        CHECK(again != nullptr);
        pool.release(again);
        pool.release(ok);
        CHECK(pool.size() == 0);
    }

    // --- non-trivial destructor actually runs ---------------------------
    {
        ObjectPool<Loud, 4> pool;
        Loud* l = pool.acquire();
        CHECK(Loud::destroyed == 0);
        pool.release(l);
        CHECK(Loud::destroyed == 1);
    }

    // --- padding / alignment edge cases ---------------------------------
    {
        ObjectPool<Tiny, 8> pool;                // sizeof(T) < sizeof(Node*)
        Tiny* v[8];
        for (int i = 0; i < 8; ++i) { v[i] = pool.acquire(); CHECK(v[i] != nullptr); }
        CHECK(pool.exhausted());
        CHECK(pool.high_water_mark() == 8);
        for (int i = 0; i < 8; ++i) pool.release(v[i]);
        CHECK(pool.size() == 0);
    }
    {
        ObjectPool<Odd, 5> pool;                 // sizeof not a multiple of align
        Odd* v[5];
        for (int i = 0; i < 5; ++i) {
            v[i] = pool.acquire();
            CHECK(reinterpret_cast<std::uintptr_t>(v[i]) % alignof(Odd) == 0);
        }
        for (int i = 0; i < 5; ++i) pool.release(v[i]);
    }

    // --- over-aligned type ----------------------------------------------
    {
        struct alignas(64) Wide { double d[8]; };
        ObjectPool<Wide, 4> pool;
        for (int i = 0; i < 4; ++i) {
            Wide* w = pool.acquire();
            CHECK(reinterpret_cast<std::uintptr_t>(w) % 64 == 0);
        }
    }

    // --- std::string: real non-trivial type -----------------------------
    {
        ObjectPool<std::string, 4> pool;
        std::string* s = pool.acquire("hello world, definitely past SSO length");
        CHECK(*s == "hello world, definitely past SSO length");
        pool.release(s);                         // must not leak
    }

    // --- stats() ---------------------------------------------------------
    {
        ObjectPool<Particle, 8> pool;
        Particle* v[5];
        for (int i = 0; i < 5; ++i) v[i] = pool.acquire(0.f, 0.f);
        for (int i = 0; i < 3; ++i) pool.release(v[i]);
        PoolStats st = pool.stats();
        CHECK(st.capacity == 8);
        CHECK(st.in_use == 2);
        CHECK(st.available == 6);
        CHECK(st.high_water_mark == 5);
        pool.release(v[3]);
        pool.release(v[4]);
    }

    // --- release(nullptr) is a no-op ------------------------------------
    {
        ObjectPool<Particle, 2> pool;
        pool.release(nullptr);
        CHECK(pool.size() == 0);
    }

    // --- compile-time properties ----------------------------------------
    static_assert(!std::is_copy_constructible_v<ObjectPool<Particle, 4>>, "must not copy");
    static_assert(!std::is_move_constructible_v<ObjectPool<Particle, 4>>, "must not move");
    static_assert(std::is_nothrow_default_constructible_v<ObjectPool<Particle, 4>>, "ctor noexcept");
    static_assert(ObjectPool<Particle, 4>::capacity() == 4, "constexpr capacity");

    if (failures == 0) std::printf("ALL SMOKE TESTS PASSED\n");
    else               std::printf("%d FAILURE(S)\n", failures);
    return failures != 0;
}
