#include <stdint.h>
#include <stdbool.h>

#ifdef ICEBREAKER
#  define MEM_TOTAL 0x20000
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
	reg_uart_clkdiv = 18000000 / 115200; 
    reg_7seg = 0x02;       // represents Demo 02
	reg_leds = 0x00;
	set_flash_qspi_flag();
    set_flash_mode_qddr(); // fastest SPI flash
}



//
// Each MIX invocation compiles to 5 distinct RISC-V instructions.
// Constants are derived from the seed n so every expansion is unique:
// the compiler cannot fold or deduplicate them.

#define MIX(a, b, c, d) \
    x = x ^ (uint8_t)(a); \
    x = x + (uint8_t)(b); \
    x = x * (uint8_t)(c); \
    x = (uint8_t)((x << 3) | (x >> 5)); \
    x = x ^ (uint8_t)(d);

#define UNIQUE(n) MIX( \
    ((n) * 7  + 11) & 0xFF, \
    ((n) * 13 + 17) & 0xFF, \
    (((n) * 19 + 23) | 1) & 0xFF, \
    ((n) * 29 + 31) & 0xFF \
)

// Boost-PP-style expansion: U1024(n) emits 1024 unique 5-instruction blocks
#define U4(n)    UNIQUE((n)+0)   UNIQUE((n)+1)   UNIQUE((n)+2)   UNIQUE((n)+3)
#define U16(n)   U4((n)+0)       U4((n)+4)       U4((n)+8)       U4((n)+12)
#define U64(n)   U16((n)+0)      U16((n)+16)     U16((n)+32)     U16((n)+48)
#define U256(n)  U64((n)+0)      U64((n)+64)     U64((n)+128)    U64((n)+192)
#define U1024(n) U256((n)+0)     U256((n)+256)   U256((n)+512)   U256((n)+768)

unsigned char run_workload(void){
    uint8_t x = 0x5A;

    // 1024 unique MIX blocks × 5 instructions
    // all executed exactly once, in strict sequential order.
    // No backward branches; the prefetch buffer streams the entire body.
    // A 4 KB or 16 KB LRU instruction cache will thrash continuously,
    // evicting lines it will never see again.
    U1024(1)

    return x;
}

void main(){
    setup_picosoc();
    reg_leds = 0x01;
    reg_uart_clkdiv = 18000000 / 115200;

    unsigned char leds_value = 0x02;
    while (1) {
        reg_7seg = run_workload();
        reg_leds = leds_value;
        leds_value = leds_value ^ 0x02;
    }
}