#pragma once
#include <cstdint>
#include <cmath>

// ── xoshiro256** PRNG ─────────────────────────────────────────────────────────
// A fast, high-quality 64-bit pseudo-random number generator.
// "xoshiro" = XOR / Shift / Rotate. The "**" variant has excellent statistical
// properties and passes all known randomness tests (BigCrush, PractRand).
// State is 256 bits (4 × uint64). NOT cryptographically secure.
struct RNG {
    uint64_t s[4];  // Internal 256-bit state; must never be all-zero

    // Seed the RNG using SplitMix64 to expand a single 64-bit seed into
    // four uncorrelated 64-bit state words. SplitMix64 is used here because
    // it is guaranteed to produce a non-zero xoshiro state for any input seed.
    explicit RNG(uint64_t seed = 12345) {
        auto sm = [](uint64_t& x) -> uint64_t {
            // SplitMix64: mix-then-advance on a counter
            x += 0x9e3779b97f4a7c15ULL;   // Knuth multiplicative constant (golden ratio)
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            return z ^ (z >> 31);
        };
        // Each call to sm() advances the counter and returns a fresh word
        s[0] = sm(seed); s[1] = sm(seed);
        s[2] = sm(seed); s[3] = sm(seed);
    }

    // Advance the state and return the next 64-bit pseudo-random value.
    // The "result" is computed from s[1] before the state update so that
    // the output and state transition are independent (improves quality).
    uint64_t next() {
        const uint64_t result = rotl(s[1] * 5, 7) * 9;  // scramble s[1]
        const uint64_t t = s[1] << 17;                   // temp for state update

        // Linear feedback shift register (LFSR) update across four words
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);

        return result;
    }

    // Returns a uniform float in [0, 1).
    // We shift right by 11 to get a 53-bit integer, then divide by 2^53,
    // matching the mantissa precision of IEEE 754 doubles (and floats).
    float uniform() {
        return (next() >> 11) * (1.0f / (1ULL << 53));
    }

    // Returns a uniform float in [lo, hi)
    float range(float lo, float hi) { return lo + uniform() * (hi - lo); }

    // Approximate normal distribution via Box-Muller transform.
    // Box-Muller converts two uniform samples into two independent standard-
    // normal values; we use one and discard the other for simplicity.
    // The 1e-7f offset prevents log(0) if uniform() returns exactly 0.
    float normal(float mean = 0.f, float stddev = 1.f) {
        float u = uniform() + 1e-7f;
        float v = uniform();
        float n = std::sqrt(-2.f * std::log(u)) * std::cos(6.2831853f * v);
        return mean + n * stddev;
    }

    // Returns true with probability p ∈ [0, 1]
    bool chance(float p) { return uniform() < p; }

private:
    // Bitwise left-rotation: moves high bits that would be lost by a left
    // shift back into the low positions, preserving all 64 bits of entropy.
    static uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
};

// ── Global RNG singleton ──────────────────────────────────────────────────────
// Seeded once from wall-clock time so each run of the program is different.
// Accessed via globalRNG() throughout the codebase; NOT thread-safe.
#include <ctime>
inline RNG& globalRNG() {
    static RNG rng(static_cast<uint64_t>(std::time(nullptr)));
    return rng;
}
