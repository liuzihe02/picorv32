// idea1.c -- scored workload for the GB3 RISC-V benchmark harness.
//
// Defines run_workload(): runs four micro-benchmarks back to back and folds
// every result into a returned checksum byte (so the optimiser can't delete
// the work). picosoc/firmware.c's run_scope()/[B] menu times this via
// time_benchmark() and prints cycles / instrs / CPI / ms over UART.
//
// Built at -O0 (the Makefile sets no -O flag) and -march=rv32im, which is what
// the "code bloat" assumptions below rely on.

#include <stdint.h>

// -----------------------------------------------------------------------------
// Benchmark 1: Capacity Crusher -- thrash the 1 KB I-cache, highlight QSPI.
// At -O0 each OP expands to load/shift/xor/store, so the unrolled body blows
// past the cache and pays the flash-fetch penalty per instruction.
// -----------------------------------------------------------------------------
#define COLD1(x)  x = x * 1000003u + 0x9E37u;
#define COLD4(x)  COLD1(x) COLD1(x) COLD1(x) COLD1(x)
#define COLD16(x) COLD4(x) COLD4(x) COLD4(x) COLD4(x)
#define COLD64(x) COLD16(x) COLD16(x) COLD16(x) COLD16(x)
#define COLD256(x) COLD64(x) COLD64(x) COLD64(x) COLD64(x)

uint8_t bench_cold_fetch(void)  // large straight-line body -> footprint > cache
{
	uint32_t x = 0xABCDEFu, acc = 0;
	for (uint32_t r = 0; r < 500u; r++) { COLD256(x) COLD256(x) COLD256(x) COLD256(x) acc ^= x; }
	return (uint8_t)(acc ^ x);
}

// -----------------------------------------------------------------------------
// Benchmark 2: Linear Multiplication -- tiny loop body (fits the I-cache),
// back-to-back multiplies to highlight fast MUL.
// -----------------------------------------------------------------------------
#define HASH_ITERATIONS 2048

static uint32_t bm2_linear_mult(uint32_t input) {
    uint32_t h = input;
    for (int i = 0; i < HASH_ITERATIONS; i++) {
        h *= 0xcc9e2d51u;
        h = (h << 15) | (h >> 17);
        h *= 0x1b873593u;
        h ^= h >> 16;
        h *= 0x85ebca6bu;
    }
    return h;
}

// -----------------------------------------------------------------------------
// Benchmark 3: SPRAM Random Accessor -- sparse random R-M-W over a 64 KB array
// to stress data memory; raw 1-cycle SPRAM should dominate.
// static (BSS in SPRAM), not on the stack. volatile so accesses aren't elided.
// -----------------------------------------------------------------------------
#define SPRAM_ELEM_COUNT 16384  // 64 KB
static volatile uint32_t spram_data[SPRAM_ELEM_COUNT];

static uint32_t bm3_random_accessor(void) {
    uint32_t lcg_state = 123456789u;
    for (int i = 0; i < 4096; i++) {
        lcg_state = (1103515245u * lcg_state + 12345u) & 0x7FFFFFFF;
        uint32_t idx = lcg_state % SPRAM_ELEM_COUNT;
        spram_data[idx] = spram_data[idx] ^ 0xDEADBEEFu;
    }
    return lcg_state ^ spram_data[lcg_state % SPRAM_ELEM_COUNT];
}

// -----------------------------------------------------------------------------
// Benchmark 4: Branch-Heavy Search -- unpredictable binary searches to defeat
// prefetch / look-ahead.
// -----------------------------------------------------------------------------
#define SEARCH_ELEM_COUNT 4096
static volatile uint32_t sorted_data[SEARCH_ELEM_COUNT];

static int binary_search(uint32_t target) {
    int left = 0, right = SEARCH_ELEM_COUNT - 1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        uint32_t val = sorted_data[mid];
        if (val == target) return mid;
        if (val < target) left = mid + 1;
        else right = mid - 1;
    }
    return -1;
}

static uint32_t bm4_branch_heavy(void) {
    uint32_t lcg_state = 987654321u;
    uint32_t acc = 0;
    for (int i = 0; i < 512; i++) {
        lcg_state = (1103515245u * lcg_state + 12345u) & 0x7FFFFFFF;
        uint32_t target = lcg_state % (SEARCH_ELEM_COUNT * 2);
        acc += (uint32_t)binary_search(target);
    }
    return acc;
}

// -----------------------------------------------------------------------------
// The scored workload: init, run all four, fold into a checksum byte.
// -----------------------------------------------------------------------------
unsigned char run_workload(void) {
    uint32_t chk = 0;

    for (int i = 0; i < SEARCH_ELEM_COUNT; i++)
        sorted_data[i] = (uint32_t)i * 2;

    bench_cold_fetch();
    chk ^= bm2_linear_mult(0x12345678u);
    chk ^= bm3_random_accessor();
    chk ^= bm4_branch_heavy();

    return (unsigned char)chk;
}
