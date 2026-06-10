#include <stdint.h>
#include <stdbool.h>

#ifdef ICEBREAKER
#  define MEM_TOTAL 0x20000 /* 128 KB */
#else
#  error "Set -DICEBREAKER when compiling this C source file"
#endif

// a pointer to this is a null pointer, but the compiler does not
// know that because "sram" is a linker symbol from sections.lds.
extern uint32_t sram;

#define reg_spictrl (*(volatile uint32_t*)0x02000000)
#define reg_uart_clkdiv (*(volatile uint32_t*)0x02000004)
#define reg_uart_data (*(volatile uint32_t*)0x02000008)
#define reg_leds (*(volatile uint8_t*)0x03000000)
#define reg_7seg (*(volatile uint8_t*)0x03000001)

// --------------------------------------------------------

extern uint32_t flashio_worker_begin;
extern uint32_t flashio_worker_end;

void flashio(uint8_t *data, int len, uint8_t wrencmd) {
	uint32_t func[&flashio_worker_end - &flashio_worker_begin];

	uint32_t *src_ptr = &flashio_worker_begin;
	uint32_t *dst_ptr = func;

	while (src_ptr != &flashio_worker_end)
		*(dst_ptr++) = *(src_ptr++);

	((void(*)(uint8_t*, uint32_t, uint32_t))func)(data, len, wrencmd);
}

#ifdef ICEBREAKER
void set_flash_qspi_flag() {
	uint8_t buffer[8];

	// Read Configuration Registers (RDCR1 35h)
	buffer[0] = 0x35;
	buffer[1] = 0x00; // rdata
	flashio(buffer, 2, 0);
	uint8_t sr2 = buffer[1];

	// Write Enable Volatile (50h) + Write Status Register 2 (31h)
	buffer[0] = 0x31;
	buffer[1] = sr2 | 2; // Enable QSPI
	flashio(buffer, 2, 0x50);
}

void set_flash_mode_spi() {
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00000000;
}

void set_flash_mode_dual() {
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00400000;
}

void set_flash_mode_quad() {
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00240000;
}

void set_flash_mode_qddr() {
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00670000;
}

void enable_flash_crm() {
	reg_spictrl |= 0x00100000;
}

void *memcpy(void *aa, const void *bb, long n) {
	char *a = aa;
	const char *b = bb;
	while (n--) *(a++) = *(b++);
	return aa;
}
#endif

// --------------------------------------------------------

// Utilities for UART
void putchar(char c){
	if (c == '\n')
		putchar('\r');
	reg_uart_data = c;
}

void print(const char *p){
	while (*p)
		putchar(*(p++));
}

void print_hex(uint32_t v, int digits){
	for (int i = 7; i >= 0; i--) {
		char c = "0123456789abcdef"[(v >> (4*i)) & 15];
		if (c == '0' && i >= digits) continue;
		putchar(c);
		digits = i;
	}
}

void print_dec(uint32_t v){ // works up to 999 only
	if (v >= 1000) {
		print(">=1000");
		return;
	}

	if      (v >= 900) { putchar('9'); v -= 900; }
	else if (v >= 800) { putchar('8'); v -= 800; }
	else if (v >= 700) { putchar('7'); v -= 700; }
	else if (v >= 600) { putchar('6'); v -= 600; }
	else if (v >= 500) { putchar('5'); v -= 500; }
	else if (v >= 400) { putchar('4'); v -= 400; }
	else if (v >= 300) { putchar('3'); v -= 300; }
	else if (v >= 200) { putchar('2'); v -= 200; }
	else if (v >= 100) { putchar('1'); v -= 100; }

	if      (v >= 90) { putchar('9'); v -= 90; }
	else if (v >= 80) { putchar('8'); v -= 80; }
	else if (v >= 70) { putchar('7'); v -= 70; }
	else if (v >= 60) { putchar('6'); v -= 60; }
	else if (v >= 50) { putchar('5'); v -= 50; }
	else if (v >= 40) { putchar('4'); v -= 40; }
	else if (v >= 30) { putchar('3'); v -= 30; }
	else if (v >= 20) { putchar('2'); v -= 20; }
	else if (v >= 10) { putchar('1'); v -= 10; }
	else putchar('0');

	if      (v >= 9) { putchar('9'); v -= 9; }
	else if (v >= 8) { putchar('8'); v -= 8; }
	else if (v >= 7) { putchar('7'); v -= 7; }
	else if (v >= 6) { putchar('6'); v -= 6; }
	else if (v >= 5) { putchar('5'); v -= 5; }
	else if (v >= 4) { putchar('4'); v -= 4; }
	else if (v >= 3) { putchar('3'); v -= 3; }
	else if (v >= 2) { putchar('2'); v -= 2; }
	else if (v >= 1) { putchar('1'); v -= 1; }
	else putchar('0');
}

void setup_picosoc(void){
	reg_uart_clkdiv = 150; // Baud = 1152060
    reg_7seg = 0x02;       // represents Demo 02
	reg_leds = 0x00;
	set_flash_qspi_flag();
}

// --------------------------------------------------------

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
unsigned char run_workload(int verbose)
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

    // The outer loop count only scales total time -- it does NOT change CPI.
    // CPI stays terrible because the unrolled body below (STRESS_256, ~6 KB of
    // straight-line code) far exceeds the 1 KB I-cache, so every pass re-misses to
    // flash; the data-dependent MIXED_OP branch adds non-sequential (jump-penalty)
    // fetches on top. Fewer/shorter passes = less wall-clock, same awful CPI.
    for (uint32_t r = 0; r < 50u; r++) {
        STRESS_256(x, lcg_state, spram_accumulator, branch_accumulator);
    }

    // Fold all independent execution paths into a single returned checksum byte
    uint32_t final_chk = x ^ lcg_state ^ spram_accumulator ^ branch_accumulator;
    unsigned char chk = (unsigned char)(final_chk ^ (final_chk >> 8) ^ (final_chk >> 16) ^ (final_chk >> 24));

    if (verbose) {
        print("Checksum: 0x");
        print_hex(chk, 2);
        putchar('\n');
    }

    return chk;
}

unsigned char run_workload_timed(){
	uint32_t cycles_begin, cycles_end;
	uint32_t instns_begin, instns_end;

	__asm__ volatile ("rdcycle %0" : "=r"(cycles_begin));
	__asm__ volatile ("rdinstret %0" : "=r"(instns_begin));

    unsigned char x = run_workload(1);//verbose as it takes already way too long to see output

	__asm__ volatile ("rdcycle %0" : "=r"(cycles_end));
	__asm__ volatile ("rdinstret %0" : "=r"(instns_end));

    print("Cycles: 0x");
    print_hex(cycles_end - cycles_begin, 8);
    putchar('\n');
    print("Instns: 0x");
    print_hex(instns_end - instns_begin, 8);
    putchar('\n');

	return x;
}

void main(){
    setup_picosoc();

    unsigned char leds_value = 0x02;

    run_workload_timed(); // for the first time, CPI measurement

    while (1) {
        // calculation that produces a unique answer
        reg_7seg = run_workload(1);     // verbose
        reg_leds = leds_value;
        leds_value = leds_value ^ 0x02; // toggle LED1
    }
}
