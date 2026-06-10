#include <stdint.h>
#include <stdbool.h>
//#define ENABLE_DIVIDE

#if defined(__has_include)
#  if __has_include("perf.h")
    // Defines REG_CACHE_HIT_COUNT, REG_CACHE_MISS_COUNT, and cache_counters_reset()
#    include "perf.h"
#  endif
#endif

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

#define RUN_TEST(test) run_test(test, #test)

// --------------------------------------------------------

extern uint32_t flashio_worker_begin;
extern uint32_t flashio_worker_end;

void flashio(uint8_t *data, int len, uint8_t wrencmd)
{
	uint32_t func[&flashio_worker_end - &flashio_worker_begin];

	uint32_t *src_ptr = &flashio_worker_begin;
	uint32_t *dst_ptr = func;

	while (src_ptr != &flashio_worker_end)
		*(dst_ptr++) = *(src_ptr++);

	((void(*)(uint8_t*, uint32_t, uint32_t))func)(data, len, wrencmd);
}

#ifdef ICEBREAKER
void set_flash_qspi_flag()
{
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

void set_flash_mode_spi()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00000000;
}

void set_flash_mode_dual()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00400000;
}

void set_flash_mode_quad()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00240000;
}

void set_flash_mode_qddr()
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00670000;
}

void enable_flash_crm()
{
	reg_spictrl |= 0x00100000;
}
void *memcpy(void *aa, const void *bb, long n) {
	char *a = aa;
	const char *b = bb;
	while (n--) *(a++) = *(b++);
	return aa;
}
#endif

void setup_picosoc(void){
	// reg_uart_clkdiv = 104; // ~115200 baud @ 12 MHz
	// reg_uart_clkdiv = 130; // ~115200 baud @ 15 MHz
    reg_uart_clkdiv = 160; // ~115200 baud @ 18.375 MHz

    reg_7seg = 0x08;
	reg_leds = 0x00;
	set_flash_qspi_flag();
	set_flash_mode_quad();
}

// Print a 32-bit number over UART as decimal (no divide/modulo needed)
void print_dec(uint32_t v) {
    static const uint32_t powers[] = {
        1000000000, 100000000, 10000000, 1000000,
        100000, 10000, 1000, 100, 10, 1
    };
    int printing = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t p = powers[i];
        int d = v / p;
        v %= p;
        if (d || printing || i == 9) {
            reg_uart_data = '0' + d;
            printing = 1;
        }
    }
}

// Print a simple label string over UART
void print_str(const char *s) {
    while (*s) reg_uart_data = *s++;
}


void print_stats(uint32_t cycles, uint32_t instns, uint32_t hits, uint32_t misses, const char *test_name) {
    // Print results over UART
    print_str("Results for "); print_str(test_name);
    print_str("\r\nrdcycle:   ");  print_dec(cycles);
    print_str("\r\nrdinstret: ");  print_dec(instns);
    print_str("\r\nCPI:       ");
    print_dec(cycles/instns);
    print_str(".");
    print_dec((cycles * 10)/instns % 10);
    uint32_t total = hits + misses;
    if (total != 0) {
        print_str("\r\nHits:      ");  print_dec(hits);
        print_str("\r\nMisses:    ");  print_dec(misses);
        print_str("\r\nTotal:     ");  print_dec(total);
        print_str("\r\nMiss rate: ");
        print_dec(misses * 100 / total);
        reg_uart_data = '.';
        uint32_t frac_pct = (misses * 1000 / total) % 10;
        print_dec(frac_pct);
        reg_uart_data = '%';
    }
    print_str("\r\n\r\n");
}

// Simulate complex control flow with many branches,
// resulting in non consecutive instruction fetches
#define JUMP __asm__ volatile ( \
    "j 1f\n\t"                               \
    ".rept 19\n\t"                           \
    "nop\n\t"                                \
    ".endr\n\t"                              \
    "1:\n\t"                                 \
    : : :                                    \
);

#define REP2(x) x x
#define REP4(x) REP2(x) REP2(x)
#define REP8(x) REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)
#define REP32(x) REP16(x) REP16(x)
#define REP64(x) REP32(x) REP32(x)
#define REP128(x) REP64(x) REP64(x)
#define REP256(x) REP128(x) REP128(x)
#define REP512(x) REP256(x) REP256(x)
#define REP1024(x) REP512(x) REP512(x)
#define REP2048(x) REP1024(x) REP1024(x)

uint32_t simulate_consecutive_instruction_fetches() {
    uint32_t x = 7, y = 3;
#ifdef ENABLE_DIVIDE
    x /= y;
#endif
    REP64(
        x *= y;
        x <<= 16;
    )
    REP256(
        x += y;
    )
    REP64(
        y -= x;
        x ^= y;
    )
    return x;
}

// Simulate a long workload with many non-consecutive instruction fetches,
// to test the instruction cache's ability to handle this pattern.
// This pattern occurs when you have a large switch statement or 
// large if-elseif-else statements.
static void simulate_non_consecutive_instruction_fetches() {
    REP256(JUMP)
    REP128(JUMP)
    REP64(JUMP)
    REP32(JUMP)
}

unsigned char run_workload() {
    uint32_t x = 0;
    for(int i = 0; i < 13; i++) {
        x += simulate_consecutive_instruction_fetches();
    }
    for(int i = 0; i < 100; i++) {
        simulate_non_consecutive_instruction_fetches();
    }
#ifdef ENABLE_DIVIDE
    return x % 255;
#else
    return x & 0xff;
#endif
}

unsigned char run_workload_timed() {
    uint32_t cycles_begin, cycles_end;
	uint32_t instns_begin, instns_end;
    uint32_t hits = 0, misses = 0;
    
#ifdef PERF_H
    cache_counters_reset();
#endif

	__asm__ volatile ("rdcycle %0" : "=r"(cycles_begin));
	__asm__ volatile ("rdinstret %0" : "=r"(instns_begin));

    unsigned char x = run_workload();

	__asm__ volatile ("rdcycle %0" : "=r"(cycles_end));
	__asm__ volatile ("rdinstret %0" : "=r"(instns_end));

#ifdef PERF_H
    hits   = REG_CACHE_HIT_COUNT;
    misses = REG_CACHE_MISS_COUNT;
#endif

    print_stats(cycles_end - cycles_begin, instns_end - instns_begin, hits, misses, "run_workload");

    return x;
}


void main()
{
    setup_picosoc();
    
    unsigned char leds_value = 0x02;
#ifdef ENABLE_DIVIDE
    run_workload_timed();
#endif
    while (1) {
        reg_7seg = run_workload(); // display
        reg_leds = leds_value;
        leds_value = leds_value ^ 0x02; // toggle LED1
    }

}
