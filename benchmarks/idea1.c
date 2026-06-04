#include <stdint.h>

// -----------------------------------------------------------------------------
// Hardware Cycle Counter
// -----------------------------------------------------------------------------
static inline uint32_t get_cycles() {
    uint32_t cycles;
    asm volatile ("rdcycle %0" : "=r" (cycles));
    return cycles;
}

// -----------------------------------------------------------------------------
// Benchmark 1: The Capacity Crusher (Thrash 8KB caches, highlight QSPI)
// -----------------------------------------------------------------------------
// At -O0, these macros will generate immense code bloat. Each OP will compile 
// into multiple instructions (load from stack, shift, xor, store to stack). 
// This guarantees we instantly blow past their 8KB cache limit, forcing them 
// into a 60-cycle SPI penalty per instruction, while you pay ~8 cycles via QSPI.
#define OP(a)   a ^= (a << 13); a ^= (a >> 17); a ^= (a << 5);
#define OP10(a) OP(a) OP(a) OP(a) OP(a) OP(a) OP(a) OP(a) OP(a) OP(a) OP(a)
#define OP100(a) OP10(a) OP10(a) OP10(a) OP10(a) OP10(a) OP10(a) OP10(a) OP10(a) OP10(a) OP10(a)

uint32_t bm1_capacity_crusher(uint32_t seed) {
    uint32_t val = seed;
    OP100(val);
    OP100(val);
    OP100(val);
    OP100(val);
    return val;
}

// -----------------------------------------------------------------------------
// Benchmark 2: Linear Multiplication (Highlight 1KB I-Cache + Fast MUL)
// -----------------------------------------------------------------------------
// Nested matrix loops are terrible at -O0 due to stack-thrashing loop counters.
// To highlight Fast-MUL, we use a flattened loop executing a polynomial hash 
// (similar to MurmurHash). This keeps the instruction footprint tiny (fits in 
// your 1KB cache) and forces consecutive 'mul' instructions.
#define HASH_ITERATIONS 2048

uint32_t bm2_linear_mult(uint32_t input) {
    uint32_t h = input;
    // A single, simple loop minimizes -O0 stack overhead while maximizing math
    for (int i = 0; i < HASH_ITERATIONS; i++) {
        h *= 0xcc9e2d51;
        h = (h << 15) | (h >> 17);
        h *= 0x1b873593;
        h ^= h >> 16;
        h *= 0x85ebca6b;
    }
    return h;
}

// -----------------------------------------------------------------------------
// Benchmark 3: SPRAM Random Accessor (Thrash D-Caches, highlight raw SPRAM)
// -----------------------------------------------------------------------------
// At -O0, their D-Cache is already stressed by stack variables. By adding 
// 64KB of sparse, random array accesses, their D-Cache will completely fail.
// Your 1-cycle bypass directly to SPRAM will dominate this test.
#define SPRAM_ELEM_COUNT 16384 // 64KB Array
volatile uint32_t spram_data[SPRAM_ELEM_COUNT];

void bm3_random_accessor() {
    uint32_t lcg_state = 123456789;
    
    for (int i = 0; i < 4096; i++) {
        // Pseudo-random index generation
        lcg_state = (1103515245 * lcg_state + 12345) & 0x7FFFFFFF;
        uint32_t idx = lcg_state % SPRAM_ELEM_COUNT;
        
        // Read, modify, write to a random location
        spram_data[idx] = spram_data[idx] ^ 0xDEADBEEF;
    }
}

// -----------------------------------------------------------------------------
// Benchmark 4: Branch-Heavy State Machine (Defeat Look-Ahead/Prefetchers)
// -----------------------------------------------------------------------------
// Unpredictable branches remain unpredictable regardless of optimization level.
// When the branch fails, their prefetcher flushes and pays a 60-cycle penalty. 
// You only pay an 8-cycle penalty.
#define SEARCH_ELEM_COUNT 4096
volatile uint32_t sorted_data[SEARCH_ELEM_COUNT];

int binary_search(uint32_t target) {
    int left = 0;
    int right = SEARCH_ELEM_COUNT - 1;
    
    while (left <= right) {
        int mid = left + (right - left) / 2;
        uint32_t val = sorted_data[mid]; 
        
        if (val == target) return mid;
        if (val < target) left = mid + 1;
        else right = mid - 1;
    }
    return -1;
}

void bm4_branch_heavy() {
    uint32_t lcg_state = 987654321;
    volatile int found_index;
    
    for (int i = 0; i < 512; i++) {
        lcg_state = (1103515245 * lcg_state + 12345) & 0x7FFFFFFF;
        uint32_t target = lcg_state % (SEARCH_ELEM_COUNT * 2);
        found_index = binary_search(target);
    }
}

// -----------------------------------------------------------------------------
// Main Execution
// -----------------------------------------------------------------------------
typedef struct {
    uint32_t bm1_cycles;
    uint32_t bm2_cycles;
    uint32_t bm3_cycles;
    uint32_t bm4_cycles;
} BenchmarkResults;

volatile BenchmarkResults results;

int main() {
    uint32_t start, end;

    // Initialization for BM4 (Search)
    for(int i=0; i<SEARCH_ELEM_COUNT; i++) {
        sorted_data[i] = i * 2;
    }

    // --- Run Benchmark 1 ---
    start = get_cycles();
    bm1_capacity_crusher(0xCAFEBABE);
    end = get_cycles();
    results.bm1_cycles = end - start;

    // --- Run Benchmark 2 ---
    start = get_cycles();
    bm2_linear_mult(0x12345678);
    end = get_cycles();
    results.bm2_cycles = end - start;

    // --- Run Benchmark 3 ---
    start = get_cycles();
    bm3_random_accessor();
    end = get_cycles();
    results.bm3_cycles = end - start;

    // --- Run Benchmark 4 ---
    start = get_cycles();
    bm4_branch_heavy();
    end = get_cycles();
    results.bm4_cycles = end - start;

    while(1) {
        asm volatile ("wfi");
    }
    
    return 0;
}