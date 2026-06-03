/*
 *  GB3 RISC-V benchmark suite (see README-GB3.md).
 *
 *  Naming: a bench_NAME is a noun (one benchmark); run_ and time_ are verbs.
 *    bench_NAME         one benchmark (a micro or a program)
 *    bench_table[]      the suite (all benchmarks)
 *    time_benchmark()   time ONE benchmark
 *    run_benchmarks()   time the whole suite -> dashboard
 *  (run_workload/run_scope/run_flashmodes live in firmware.c.)
 *
 *  -nostdlib / -march=rv32im: 32-bit only (no libgcc 64-bit helpers).
 */

#ifndef BENCHMARKS_H
#define BENCHMARKS_H

#include <stdint.h>

// ---- System configuration ----
// TODO: Update F_CLK_HZ whenever you change the PLL (see icebreaker.v SB_PLL40_PAD)
// these global variables will be used everywhere
#define F_CLK_HZ 17250000u   // PLL system clock, Hz
#define BAUD     115200u     // UART console baud

typedef uint8_t (*bench_fn)(void);

// group: 0 = Compute, 1 = Memory, 2 = Fetch, 3 = Programs
typedef struct {
	const char *name;
	uint8_t     group;
	const char *hint;   // static op-mix hint (from the sim profiler); "" if none
	bench_fn    fn;
} bench_t;

extern const bench_t bench_table[];
extern const int      bench_count;

// time one benchmark: returns cycles, outputs retired instrs and the checksum
uint32_t time_benchmark(bench_fn fn, uint32_t *instrs, uint8_t *chk);

// time the whole suite and print the dashboard over UART
void run_benchmarks(void);

// print CPI (cycles/instrs) to 2 decimals via integer math, e.g. "4.27"
void print_cpi(uint32_t cyc, uint32_t ins);

// print wall-clock ms (3 decimals) for a cycle count.
void print_ms(uint32_t cyc);

// the benchmarks (Compute / Memory / Fetch / Programs)
uint8_t bench_alu(void);
uint8_t bench_shift(void);
uint8_t bench_mul(void);
uint8_t bench_div(void);
uint8_t bench_branch(void);
uint8_t bench_call(void);
uint8_t bench_memcpy(void);
uint8_t bench_chase(void);
uint8_t bench_hot(void);
uint8_t bench_cold(void);

uint8_t bench_bubble_sort(void);
uint8_t bench_matmul(void);
uint8_t bench_crc32(void);
uint8_t bench_prime_count(void);
uint8_t bench_fir(void);
uint8_t bench_strsearch(void);
uint8_t bench_interp(void);
uint8_t bench_game_of_life(void);
uint8_t bench_fib_rec(void);
uint8_t bench_xorshift_mc(void);

#endif
