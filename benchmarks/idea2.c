// consolidated_workload.c -- Single-pass micro-architectural stress test
// Compiles at -O0 for rv32im. Targets I-Cache, SPRAM, and branch prediction.

#include <stdint.h>

#define SPRAM_ELEM_COUNT 16384  // 64 KB SPRAM array
#define SEARCH_ELEM_COUNT 1024  // Reduced size so data structures don't overflow small memory footprint

static volatile uint32_t spram_data[SPRAM_ELEM_COUNT];
static volatile uint32_t sorted_data[SEARCH_ELEM_COUNT];

// -----------------------------------------------------------------------------
// Core Interleaved Macro
// Each invocation executes:
//  1. The original linear multiplication (Benchmark 1 & 2)
//  2. An LCG update to generate pseudo-random patterns on the fly
//  3. An unpredictable Read-Modify-Write into SPRAM (Benchmark 3)
//  4. A data-dependent conditional branch (Benchmark 4)
// -----------------------------------------------------------------------------
#define MIXED_OP(x, lcg, spram_acc, branch_acc) do { \
    /* 1. Linear Mult / Code Bloat */ \
    x = x * 1000003u + 0x9E37u; \
    /* 2. Fast LCG update */ \
    lcg = (1103515245u * lcg + 12345u) & 0x7FFFFFFF; \
    /* 3. SPRAM Random Accessor (Masking instead of modulo for speed at -O0) */ \
    uint32_t idx = lcg & (SPRAM_ELEM_COUNT - 1); \
    spram_data[idx] ^= x; \
    spram_acc ^= spram_data[idx]; \
    /* 4. Branching Behavior (Unpredictable condition based on LCG state) */ \
    if (lcg & 0x100) { \
        branch_acc += sorted_data[lcg & (SEARCH_ELEM_COUNT - 1)]; \
    } else { \
        branch_acc -= x; \
    } \
} while(0)

// Unrolling the consolidated step to blow past the 1 KB I-cache
#define STRESS_4(x, l, s, b)   MIXED_OP(x, l, s, b); MIXED_OP(x, l, s, b); MIXED_OP(x, l, s, b); MIXED_OP(x, l, s, b);
#define STRESS_16(x, l, s, b)  STRESS_4(x, l, s, b)  STRESS_4(x, l, s, b)  STRESS_4(x, l, s, b)  STRESS_4(x, l, s, b);
#define STRESS_64(x, l, s, b)  STRESS_16(x, l, s, b) STRESS_16(x, l, s, b) STRESS_16(x, l, s, b) STRESS_16(x, l, s, b);
#define STRESS_256(x, l, s, b) STRESS_64(x, l, s, b) STRESS_64(x, l, s, b) STRESS_64(x, l, s, b) STRESS_64(x, l, s, b);
#define STRESS_1024(x, l, s, b) STRESS_256(x, l, s, b) STRESS_256(x, l, s, b) STRESS_256(x, l, s, b) STRESS_256(x, l, s, b);

// -----------------------------------------------------------------------------
// Execution Engine
// -----------------------------------------------------------------------------
unsigned char run_workload(void)
{
    // Initialize data structures
    for (int i = 0; i < SEARCH_ELEM_COUNT; i++) {
        sorted_data[i] = (uint32_t)i * 2;
    }
    for (int i = 0; i < SPRAM_ELEM_COUNT; i++) {
        spram_data[i] = 0x5A5A5A5Au;
    }

    // State tracking variables
    uint32_t x = 0xABCDEFu;
    uint32_t lcg_state = 123456789u;
    uint32_t spram_accumulator = 0;
    uint32_t branch_accumulator = 0;

    // The outer loop keeps the execution window open long enough for accurate timing.
    // 1024 macro operations per loop iteration * 200 iterations = 204,800 operations.
    // At -O0, this guarantees structural footprint saturation and high loop counts.
    for (uint32_t r = 0; r < 200u; r++) {
        STRESS_1024(x, lcg_state, spram_accumulator, branch_accumulator);
        STRESS_1024(x, lcg_state, spram_accumulator, branch_accumulator);
        STRESS_1024(x, lcg_state, spram_accumulator, branch_accumulator);
    }

    // Fold all independent execution paths into a single returned checksum byte
    uint32_t final_chk = x ^ lcg_state ^ spram_accumulator ^ branch_accumulator;
    return (unsigned char)(final_chk ^ (final_chk >> 8) ^ (final_chk >> 16) ^ (final_chk >> 24));
}