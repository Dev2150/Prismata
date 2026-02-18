#pragma once
#include <cstdint>
#include <cmath>

// xoshiro256** – fast, high-quality 64-bit PRNG
struct RNG {
    uint64_t s[4];

    explicit RNG(uint64_t seed = 12345) {
        // SplitMix64 to initialise state from a single seed
        auto sm = [](uint64_t& x) -> uint64_t {
            x += 0x9e3779b97f4a7c15ULL;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        };
        s[0] = sm(seed); s[1] = sm(seed);
        s[2] = sm(seed); s[3] = sm(seed);
    }

    uint64_t next() {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1];
        s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    // [0, 1)
    float uniform() {
        return (next() >> 11) * (1.0f / (1ULL << 53));
    }

    // [lo, hi)
    float range(float lo, float hi) { return lo + uniform() * (hi - lo); }

    // Approximate Box-Muller normal distribution
    float normal(float mean = 0.f, float stddev = 1.f) {
        float u = uniform() + 1e-7f;
        float v = uniform();
        float n = std::sqrt(-2.f * std::log(u)) * std::cos(6.2831853f * v);
        return mean + n * stddev;
    }

    bool chance(float p) { return uniform() < p; }

private:
    static uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
};

// One global RNG – seed from time at startup
#include <ctime>
inline RNG& globalRNG() {
    static RNG rng(static_cast<uint64_t>(std::time(nullptr)));
    return rng;
}
