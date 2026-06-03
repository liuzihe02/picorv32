/*
 *  GB3 RISC-V benchmark suite -- see benchmarks.h for the naming convention.
 *
 *  Every benchmark folds all its work into a returned byte checksum (so the
 *  optimiser can't delete it) and uses a fixed seed (deterministic N / cycles
 *  / checksum). Loop counts are tuned for fast iverilog sim; raise them for
 *  longer on-board runs.
 */

#include <stdint.h>
#include "benchmarks.h"

// I/O helpers defined in firmware.c
extern void putchar(char c);
extern void print(const char *p);
extern void print_dec(uint32_t v);

// --- timing primitive ---------------------------------------------------

uint32_t time_benchmark(bench_fn fn, uint32_t *instrs, uint8_t *chk)
{
	uint32_t c0, c1, i0, i1;
	__asm__ volatile ("rdcycle %0"   : "=r"(c0));
	__asm__ volatile ("rdinstret %0" : "=r"(i0));
	uint8_t r = fn();
	__asm__ volatile ("rdinstret %0" : "=r"(i1));
	__asm__ volatile ("rdcycle %0"   : "=r"(c1));
	if (instrs) *instrs = i1 - i0;
	if (chk)    *chk    = r;
	return c1 - c0;
}

// --- number formatting (no float / libgcc, no 64-bit divide) ------------
// Each fmt_* writes a NUL-terminated string into the caller's buffer `b` (no static
// state -> reentrant). Caller sizes it: >=11 for fmt_u32, >=16 for fmt_cpi/fmt_ms.

// uint32 -> decimal in `b`; returns digit count. Digits come out LSB-first, so
// stage in `t` and reverse.
static int fmt_u32(uint32_t v, char *b)
{
	char t[10]; int n = 0;
	if (!v) { b[0] = '0'; b[1] = 0; return 1; }
	while (v) { t[n++] = '0' + v % 10; v /= 10; }
	for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
	b[n] = 0;
	return n;
}

// CPI as "X.YY" into `b` ("--" if ins==0). Integer-only: frac = (cyc%ins)*100/ins,
// not cyc*100/ins, which overflows uint32 once cyc > ~43M.
static void fmt_cpi(uint32_t cyc, uint32_t ins, char *b)
{
	if (!ins) { b[0] = b[1] = '-'; b[2] = 0; return; }
	int k = fmt_u32(cyc / ins, b);
	uint32_t f = ((cyc % ins) * 100u) / ins;
	b[k++] = '.'; b[k++] = '0' + f / 10; b[k++] = '0' + f % 10; b[k] = 0;
}

// Wall-clock "X.YYY" ms into `b`. f_clk = F_CLK_HZ (benchmarks.h, also sets the UART
// divisor -> tracks the PLL). Work in kHz to keep the math in uint32.
static void fmt_ms(uint32_t cyc, char *b)
{
	uint32_t khz = F_CLK_HZ / 1000u;   // e.g. 17625
	if (!khz) { b[0] = b[1] = '-'; b[2] = 0; return; }
	int k = fmt_u32(cyc / khz, b);
	uint32_t f = ((cyc % khz) * 1000u) / khz;
	b[k++] = '.'; b[k++] = '0' + f / 100; b[k++] = '0' + (f / 10) % 10; b[k++] = '0' + f % 10; b[k] = 0;
}

// Format-and-print wrappers (used by run_scope); the dashboard calls fmt_* directly to pad.
void print_cpi(uint32_t cyc, uint32_t ins) { char b[16]; fmt_cpi(cyc, ins, b); print(b); }
void print_ms(uint32_t cyc)                { char b[16]; fmt_ms(cyc, b);       print(b); }

// --- deterministic PRNG (fixed seed) ------------------------------------

static uint32_t rng_s;
static inline uint32_t rng(void) { uint32_t x = rng_s; x ^= x<<13; x ^= x>>17; x ^= x<<5; return rng_s = x; }
static inline void seed(void)    { rng_s = 0x1234567u; }

// ===== Compute ==========================================================

uint8_t bench_alu(void)
{
	uint32_t a = 0x9E3779B9u, b = 0x12345678u, acc = 0;
	for (uint32_t i = 0; i < 50000u; i++) {
		a = a + b; a ^= b - i; a |= i & 0x55; a &= 0xFFFFFF3F;
		acc += (a < b);                                  // slt
		b = a - b;
	}
	return (uint8_t)(acc ^ a ^ b);
}

uint8_t bench_shift(void)
{
	uint32_t a = 0xCAFEBABEu, acc = 0;
	for (uint32_t i = 0; i < 50000u; i++) {
		uint32_t s = i & 31;
		a = (a << s) | (a >> ((32 - s) & 31));           // rotate
		a ^= (uint32_t)((int32_t)a >> (s & 15));         // sra
		acc += a >> (s & 7);                             // srl
	}
	return (uint8_t)(acc ^ a);
}

uint8_t bench_mul(void)
{
	uint32_t a = 0x1000193u, b = 0x9E3779B1u, acc = 0;
	for (uint32_t i = 0; i < 50000u; i++) {
		a = a * b + i;                                   // mul
		acc += (uint32_t)(((uint64_t)a * b) >> 32);      // mulhu (single widening mul)
		b = b * 1103515245u + 12345u;
	}
	return (uint8_t)(acc ^ a ^ b);
}

uint8_t bench_div(void)
{
	uint32_t x = 0xDEADBEEFu, acc = 0;
	for (uint32_t i = 1; i <= 40000u; i++) {
		uint32_t d = (i & 0xFFFF) | 1;                   // nonzero divisor
		acc += x / d;                                    // divu
		acc ^= x % d;                                    // remu
		x = x * 2654435761u + i;
	}
	return (uint8_t)(acc ^ x);
}

uint8_t bench_branch(void)
{
	seed();
	uint32_t acc = 0;
	for (uint32_t i = 0; i < 50000u; i++) {
		uint32_t r = rng();                              // unpredictable
		if (r & 1) acc += 3; else acc -= 1;
		if ((r & 6) == 0) acc ^= r;
		if (r > acc) acc += r & 0xFF; else acc ^= 0x5A;
		if ((int32_t)r < 0) acc--;
	}
	return (uint8_t)acc;
}

static uint32_t __attribute__((noinline)) leaf(uint32_t a, uint32_t b)
{
	return (a ^ (b << 1)) + (b ^ (a >> 1));
}

uint8_t bench_call(void)
{
	uint32_t acc = 0, a = 1, b = 2;
	for (uint32_t i = 0; i < 50000u; i++) {
		acc += leaf(a, b);
		a = leaf(b, i);
		b = leaf(acc, a);
	}
	return (uint8_t)acc;
}

// ===== Memory ===========================================================

static uint32_t mcs[256], mcd[256];

uint8_t bench_memcpy(void)
{
	for (int i = 0; i < 256; i++) mcs[i] = (uint32_t)i * 2654435761u;
	uint32_t acc = 0;
	for (uint32_t r = 0; r < 200u; r++) {
		for (int i = 0; i < 256; i++) mcd[i] = mcs[i] + r;   // load + store
		acc ^= mcd[(r * 7) & 255];
	}
	return (uint8_t)acc;
}

static uint32_t chase[256];

uint8_t bench_chase(void)
{
	for (int i = 0; i < 256; i++) chase[i] = (uint32_t)((i * 167 + 13) & 255);
	uint32_t p = 0, acc = 0;
	for (uint32_t i = 0; i < 50000u; i++) { p = chase[p]; acc += p; }  // dependent loads
	return (uint8_t)(acc ^ p);
}

// ===== Fetch ============================================================

uint8_t bench_hot(void)   // tiny loop body -> fits any cache / loop buffer
{
	uint32_t a = 0x12345u, acc = 0;
	for (uint32_t i = 0; i < 200000u; i++) { a = a * 1664525u + 1013904223u; acc ^= a >> 24; }
	return (uint8_t)acc;
}

#define COLD1(x)  x = x * 1000003u + 0x9E37u;
#define COLD4(x)  COLD1(x) COLD1(x) COLD1(x) COLD1(x)
#define COLD16(x) COLD4(x) COLD4(x) COLD4(x) COLD4(x)
#define COLD64(x) COLD16(x) COLD16(x) COLD16(x) COLD16(x)

uint8_t bench_cold(void)  // large straight-line body -> footprint > cache
{
	uint32_t x = 0xABCDEFu, acc = 0;
	for (uint32_t r = 0; r < 500u; r++) { COLD64(x) COLD64(x) COLD64(x) COLD64(x) acc ^= x; }
	return (uint8_t)(acc ^ x);
}

// ===== Programs =========================================================

static int32_t sbuf[64];

uint8_t bench_bubble_sort(void)
{
	seed();
	uint32_t acc = 0;
	for (uint32_t r = 0; r < 10u; r++) {
		for (int i = 0; i < 64; i++) sbuf[i] = (int32_t)rng();
		for (int i = 0; i < 63; i++)
			for (int j = 0; j < 63 - i; j++)
				if (sbuf[j] > sbuf[j+1]) { int32_t t = sbuf[j]; sbuf[j] = sbuf[j+1]; sbuf[j+1] = t; }
		acc += (uint32_t)sbuf[0] ^ (uint32_t)sbuf[63];
	}
	return (uint8_t)acc;
}

static int16_t mA[256], mB[256];
static int32_t mC[256];

uint8_t bench_matmul(void)   // 16x16 integer matrix multiply
{
	seed();
	uint32_t acc = 0;
	for (uint32_t r = 0; r < 20u; r++) {
		for (int i = 0; i < 256; i++) { mA[i] = (int16_t)rng(); mB[i] = (int16_t)rng(); }
		for (int i = 0; i < 16; i++)
			for (int j = 0; j < 16; j++) {
				int32_t s = 0;
				for (int k = 0; k < 16; k++) s += (int32_t)mA[i*16+k] * mB[k*16+j];
				mC[i*16+j] = s;
			}
		acc += (uint32_t)mC[0] ^ (uint32_t)mC[255];
	}
	return (uint8_t)acc;
}

static uint8_t cbuf[1024];

uint8_t bench_crc32(void)
{
	seed();
	for (int i = 0; i < 1024; i++) cbuf[i] = (uint8_t)rng();
	uint32_t acc = 0;
	for (uint32_t r = 0; r < 20u; r++) {
		uint32_t crc = 0xFFFFFFFFu;
		for (int i = 0; i < 1024; i++) {
			crc ^= cbuf[i];
			for (int b = 0; b < 8; b++)
				crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));   // branchless
		}
		acc ^= crc;
	}
	return (uint8_t)acc;
}

uint8_t bench_prime_count(void)   // primes < 2000 by trial division
{
	uint32_t acc = 0;
	for (uint32_t r = 0; r < 3u; r++) {
		uint32_t count = 0;
		for (uint32_t n = 2; n < 2000u; n++) {
			int prime = 1;
			for (uint32_t d = 2; d * d <= n; d++) if (n % d == 0) { prime = 0; break; }
			if (prime) count++;
		}
		acc += count;
	}
	return (uint8_t)acc;
}

static int16_t sig[1040], coef[16];

uint8_t bench_fir(void)   // fixed-point 16-tap FIR
{
	seed();
	for (int i = 0; i < 1040; i++) sig[i]  = (int16_t)rng();
	for (int i = 0; i < 16;   i++) coef[i] = (int16_t)((rng() & 0xFF) - 128);
	uint32_t acc = 0;
	for (uint32_t r = 0; r < 10u; r++)
		for (int n = 0; n < 1024; n++) {
			int32_t y = 0;
			for (int k = 0; k < 16; k++) y += (int32_t)coef[k] * sig[n+k];
			acc += (uint32_t)(y >> 8);
		}
	return (uint8_t)acc;
}

static char text[2048];

uint8_t bench_strsearch(void)   // naive substring search
{
	seed();
	for (int i = 0; i < 2048; i++) text[i] = 'a' + (char)(rng() & 3);
	static const char pat[] = "acab";
	const int plen = 4;
	uint32_t acc = 0;
	for (uint32_t r = 0; r < 20u; r++) {
		uint32_t hits = 0;
		for (int i = 0; i + plen <= 2048; i++) {
			int j = 0;
			while (j < plen && text[i+j] == pat[j]) j++;
			if (j == plen) hits++;
		}
		acc += hits;
	}
	return (uint8_t)acc;
}

// --- the suite ----------------------------------------------------------

const bench_t bench_table[] = {
	{ "alu",         0, "",             bench_alu },
	{ "shift",       0, "",             bench_shift },
	{ "mul",         0, "",             bench_mul },
	{ "div",         0, "",             bench_div },
	{ "branch",      0, "",             bench_branch },
	{ "call",        0, "",             bench_call },
	{ "memcpy",      1, "",             bench_memcpy },
	{ "chase",       1, "",             bench_chase },
	{ "hot",         2, "",             bench_hot },
	{ "cold",        2, "",             bench_cold },
	{ "bubble_sort", 3, "branch/ld-st", bench_bubble_sort },
	{ "matmul",      3, "mul/ld-st",    bench_matmul },
	{ "crc32",       3, "shift/xor",    bench_crc32 },
	{ "prime_count", 3, "div/branch",   bench_prime_count },
	{ "fir",         3, "mul/shift",    bench_fir },
	{ "strsearch",   3, "ld/branch",    bench_strsearch },
};
const int bench_count = (int)(sizeof(bench_table) / sizeof(bench_table[0]));

static const char *group_name[4] = { "Compute", "Memory", "Fetch", "Programs" };

// left-align s in width w (trailing spaces)
static void lcol(const char *s, int w)
{
	int n = 0;
	while (s[n]) { putchar(s[n]); n++; }
	while (n < w) { putchar(' '); n++; }
}

// right-align s in width w (leading spaces)
static void rcol(const char *s, int w)
{
	int n = 0; while (s[n]) n++;
	while (n < w) { putchar(' '); n++; }
	print(s);
}

static void dashes(int n) { while (n-- > 0) putchar('-'); }

// Aligned columns separated by " | "; a matching "-+-" rule sits under the header.
void run_benchmarks(void)
{
	char buf[16];

	// clock line: print F_CLK_HZ in kHz via fmt_u32, then insert a '.' 3 digits from
	// the right -> "XX.YYY MHz" (kHz resolution). fmt_u32's length tells us where.
	int n = fmt_u32(F_CLK_HZ / 1000u, buf);     // e.g. 16875
	print("\nclock = ");
	for (int i = 0; i < n - 3; i++) putchar(buf[i]);
	putchar('.');
	for (int i = n - 3; i < n; i++) putchar(buf[i]);
	print(" MHz\n\n");

	lcol("benchmark", 13);      print(" | ");
	rcol("cycles", 10);         print(" | ");
	rcol("instrs", 8);          print(" | ");
	rcol("CPI", 6);             print(" | ");
	rcol("wallclock (ms)", 14); print(" | ");
	rcol("chk", 3);             print(" | mix\n");
	dashes(13); print("-+-"); dashes(10); print("-+-"); dashes(8);  print("-+-");
	dashes(6);  print("-+-"); dashes(14); print("-+-"); dashes(3);  print("-+-"); dashes(3); print("\n");

	int g = -1;
	for (int i = 0; i < bench_count; i++) {
		const bench_t *b = &bench_table[i];
		if (b->group != g) { g = b->group; print("-- "); print(group_name[g]); print(" --\n"); }

		uint32_t ins;
		uint8_t  chk;
		uint32_t cyc = time_benchmark(b->fn, &ins, &chk);

		lcol(b->name, 13);                        print(" | ");
		fmt_u32(cyc, buf);      rcol(buf, 10);    print(" | ");
		fmt_u32(ins, buf);      rcol(buf, 8);     print(" | ");
		fmt_cpi(cyc, ins, buf); rcol(buf, 6);     print(" | ");
		fmt_ms(cyc, buf);       rcol(buf, 14);    print(" | ");
		fmt_u32(chk, buf);      rcol(buf, 3);     print(" | ");
		print(b->hint);
		print("\n");
	}
	print("done\n");
}
