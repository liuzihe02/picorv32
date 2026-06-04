// idea3.c -- maximal QSPI-advantage workload: a pure sequential instruction-fetch flood.
//
// STRATEGY (see README-GB3 Fetch section):
//   Your edge over a cache-only / single-SPI team is ONE thing: cache-missing
//   instruction fetch from flash, where qddr (0xED) streams ~4 SPI data clocks/word
//   vs single-SPI's (0x03) ~32 -- up to an 8x gap. To collect the *full* 8x on the
//   whole score, ~100% of execution time must be exactly that. Three rules follow:
//
//   1. SEQUENTIAL, not jumpy. The flash command byte is always sent single-lane
//      (spimemio state 4, before din_qspi is set), so every non-sequential fetch
//      (taken branch / call / indirect jump) re-pays that 8-clock single-lane
//      command -- a fixed cost identical on both machines that DILUTES the quad
//      advantage (~2.7x on a jump vs ~8x while streaming). So: straight-line code,
//      no branches, no calls, no switch/jump-table in the hot body.
//
//   2. ZERO data memory. Every load/store hits 1-cycle SPRAM at ~1x ratio on every
//      machine -- pure dilution. That includes -O0 stack spills of C locals
//      (`x = x*C+K` => lw/…/sw every statement), which is what caps bench_cold at
//      ~3.7x of the 8x ceiling. The chain below lives entirely in registers via
//      inline-asm "+r" constraints: no ld/st in the hot body at all.
//
//   3. CHEAPEST core op. fetch latency dominates, but the core cycles still count
//      on both machines, so use only 3-cycle ALU ops (add/sub/xor/and/or). NO mul
//      (~5 PCPI cyc) and NO shift (iterative, up to ~14 cyc with BARREL_SHIFTER=0):
//      both add equal core cycles on both machines and shrink the ratio.
//
// FOOTPRINT: the whole iCE40UP5K has 30 EBR = 120 Kbit = 15 KB, ~13 KB free, so NO
//   competitor cache can exceed ~13 KB. The ~64 KB body below is a comfortable ~4x
//   over that -- every word misses every possible cache, on their machine and ours,
//   so both stream from flash and the 8x lane-rate gap applies to the entire run.
//   Bigger buys nothing (miss rate is already ~100%); it only lengthens the run.
//
// CEILING: ~8x (32 single-SPI data clocks vs ~4 qddr) is set by the SPI lane rate,
//   not the access pattern -- no code trick beats it. idea3 just gets as close to it
//   as physically possible by making the benchmark nothing but streaming fetch.
//
// Built -O0 / -march=rv32im (Makefile sets no -O). Timed by firmware.c run_scope()/[B].

#include <stdint.h>

// One unit = 4 single-cycle ALU ops on 3 live registers (%0=a, %1=b, %2=c), wired as
// a cyclic dependency (a->c->b->a) so it can't be reordered, folded, or elided, and
// never collapses to a fixed point. 1 instruction = 1 fetch = 4 bytes, no memory.
#define U1 \
    "add %0, %0, %1\n\t" \
    "xor %2, %2, %0\n\t" \
    "add %1, %1, %2\n\t" \
    "xor %0, %0, %1\n\t"
#define U4    U1  U1  U1  U1
#define U16   U4  U4  U4  U4
#define U64   U16 U16 U16 U16
#define U256  U64 U64 U64 U64
#define U1K   U256 U256 U256 U256   //  4096 instrs  ~16 KB
#define U4K   U1K  U1K  U1K  U1K    // 16384 instrs  ~64 KB  (~4x the 13 KB max cache)

unsigned char run_workload(void)
{
    uint32_t a = 0x12345678u, b = 0x9E3779B9u, c = 0xCAFEBABEu;

    // Outer loop only re-streams the cold body for a clean timing window. Its single
    // back-edge is the only branch in the whole workload: 1 restart per ~16k-instr
    // pass -- negligible. The body itself is one straight-line basic block, so the
    // PC marches sequentially through ~64 KB of flash that no cache can hold.
    for (uint32_t r = 0; r < 256u; r++) {
        __asm__ volatile (
            U4K
            : "+r"(a), "+r"(b), "+r"(c)
            :
            : /* no memory clobber: nothing here touches memory */
        );
    }

    // Fold the live registers to a checksum byte so the work is provably observable.
    uint32_t chk = a ^ b ^ c;
    return (unsigned char)(chk ^ (chk >> 8) ^ (chk >> 16) ^ (chk >> 24));
}
