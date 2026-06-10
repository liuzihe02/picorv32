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
	reg_uart_clkdiv = 163; //163; // Baud ~= 115200 for 12 MHz
	reg_7seg = 0x05;       // represents Demo 02
	reg_leds = 0x00;
	set_flash_qspi_flag();
	//set_flash_mode_qddr(); // fastest SPI flash
}

// --------------------------------------------------------
// Polynomial*sine numerical integral benchmark
// 19 Polynomials (some terms multiplied by sin) over a 48x48 numerical integral
// Expected checksum 0x9b
//
// Workload:
//   sum over x,y of many small polynomial kernels.
//   Some polynomial terms are multiplied by values from a sine LUT.
//
// Justification:
//   The hot instruction footprint is deliberately large to fill our large cache
//   64x2x16 = 2048 instructions,
//   (which is largest clean power of 2 cache that will fit on the BRAM)
//
//   Disassembly: Approx 1700 instructions in entire hot loop
//                 412 in main body (preparing to call the polynomials, setting up LUT access)
//                +1290 in polynomial functions
// 
//   We use sine_lut for a lot of lw instructions (take advantage of
//   dmem lookahead). Around 26% are load instructions.
// 
//   This particular combination also has set-associative cache
//   beating direct-mapped cache by a lot.
//
//   Results: 
//        Full setup: 64x2x16 associative, data lookahead 4.7135 CPI
//        Missing data lookahead:                         4.9738 CPI
//        Non-associative:                                9.5077 CPI
//        Half-size cache 32x2x16:                       13.4983 CPI
//   As such, this benchmark stress-tests the cache and memory system
//   effectively, and any missing 'feature' would be punished

//   N.B. only first 19 polynomials are used, but keep the rest because it pads
//   the assembly values such that a non-associative (direct mapped) cache struggles badly...

#define NX 48
#define NY 48

static const int8_t sine_lut[256] = {
       0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3
};

__attribute__((noinline, used))
static uint32_t poly_00(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 2*x2 + 1*y2 + 1*xy + 3*x + 5*y + 17;
    int32_t t = (1*(int32_t)x + 2*(int32_t)y + 7) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z)); // prevent too clever assembly
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_01(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 5*x2 + 6*y2 + 8*xy + 14*x + 18*y + 54;
    int32_t t = (4*(int32_t)x + 7*(int32_t)y + 14) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_02(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 8*x2 + 11*y2 + 2*xy + 25*x + 31*y + 91;
    int32_t t = (7*(int32_t)x + 12*(int32_t)y + 21) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_03(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 11*x2 + 16*y2 + 9*xy + 13*x + 15*y + 128;
    int32_t t = (10*(int32_t)x + 4*(int32_t)y + 11) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_04(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 14*x2 + 2*y2 + 3*xy + 24*x + 28*y + 165;
    int32_t t = (2*(int32_t)x + 9*(int32_t)y + 18) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_05(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 17*x2 + 7*y2 + 10*xy + 12*x + 12*y + 202;
    int32_t t = (5*(int32_t)x + 14*(int32_t)y + 8) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_06(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 3*x2 + 12*y2 + 4*xy + 23*x + 25*y + 239;
    int32_t t = (8*(int32_t)x + 6*(int32_t)y + 15) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_07(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 6*x2 + 17*y2 + 11*xy + 11*x + 9*y + 276;
    int32_t t = (11*(int32_t)x + 11*(int32_t)y + 22) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_08(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 9*x2 + 3*y2 + 5*xy + 22*x + 22*y + 313;
    int32_t t = (3*(int32_t)x + 3*(int32_t)y + 12) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_09(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 12*x2 + 8*y2 + 12*xy + 10*x + 6*y + 350;
    int32_t t = (6*(int32_t)x + 8*(int32_t)y + 19) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_10(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 15*x2 + 13*y2 + 6*xy + 21*x + 19*y + 387;
    int32_t t = (9*(int32_t)x + 13*(int32_t)y + 9) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_11(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 18*x2 + 18*y2 + 13*xy + 9*x + 32*y + 424;
    int32_t t = (1*(int32_t)x + 5*(int32_t)y + 16) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_12(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 4*x2 + 4*y2 + 7*xy + 20*x + 16*y + 461;
    int32_t t = (4*(int32_t)x + 10*(int32_t)y + 23) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_13(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 7*x2 + 9*y2 + 1*xy + 8*x + 29*y + 498;
    int32_t t = (7*(int32_t)x + 2*(int32_t)y + 13) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_14(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 10*x2 + 14*y2 + 8*xy + 19*x + 13*y + 535;
    int32_t t = (10*(int32_t)x + 7*(int32_t)y + 20) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_15(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 13*x2 + 19*y2 + 2*xy + 7*x + 26*y + 572;
    int32_t t = (2*(int32_t)x + 12*(int32_t)y + 10) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_16(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 16*x2 + 5*y2 + 9*xy + 18*x + 10*y + 609;
    int32_t t = (5*(int32_t)x + 4*(int32_t)y + 17) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_17(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 2*x2 + 10*y2 + 3*xy + 6*x + 23*y + 646;
    int32_t t = (8*(int32_t)x + 9*(int32_t)y + 7) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_18(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 5*x2 + 15*y2 + 10*xy + 17*x + 7*y + 683;
    int32_t t = (11*(int32_t)x + 14*(int32_t)y + 14) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_19(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 8*x2 + 1*y2 + 4*xy + 5*x + 20*y + 720;
    int32_t t = (3*(int32_t)x + 6*(int32_t)y + 21) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_20(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 11*x2 + 6*y2 + 11*xy + 16*x + 33*y + 757;
    int32_t t = (6*(int32_t)x + 11*(int32_t)y + 11) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_21(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 14*x2 + 11*y2 + 5*xy + 4*x + 17*y + 794;
    int32_t t = (9*(int32_t)x + 3*(int32_t)y + 18) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_22(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 17*x2 + 16*y2 + 12*xy + 15*x + 30*y + 831;
    int32_t t = (1*(int32_t)x + 8*(int32_t)y + 8) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_23(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 3*x2 + 2*y2 + 6*xy + 3*x + 14*y + 868;
    int32_t t = (4*(int32_t)x + 13*(int32_t)y + 15) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_24(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 6*x2 + 7*y2 + 13*xy + 14*x + 27*y + 905;
    int32_t t = (7*(int32_t)x + 5*(int32_t)y + 22) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_25(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 9*x2 + 12*y2 + 7*xy + 25*x + 11*y + 942;
    int32_t t = (10*(int32_t)x + 10*(int32_t)y + 12) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_26(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 12*x2 + 17*y2 + 1*xy + 13*x + 24*y + 979;
    int32_t t = (2*(int32_t)x + 2*(int32_t)y + 19) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_27(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 15*x2 + 3*y2 + 8*xy + 24*x + 8*y + 1016;
    int32_t t = (5*(int32_t)x + 7*(int32_t)y + 9) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_28(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 18*x2 + 8*y2 + 2*xy + 12*x + 21*y + 1053;
    int32_t t = (8*(int32_t)x + 12*(int32_t)y + 16) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_29(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 4*x2 + 13*y2 + 9*xy + 23*x + 5*y + 1090;
    int32_t t = (11*(int32_t)x + 4*(int32_t)y + 23) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_30(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 7*x2 + 18*y2 + 3*xy + 11*x + 18*y + 1127;
    int32_t t = (3*(int32_t)x + 9*(int32_t)y + 13) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_31(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 10*x2 + 4*y2 + 10*xy + 22*x + 31*y + 1164;
    int32_t t = (6*(int32_t)x + 14*(int32_t)y + 20) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_32(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 13*x2 + 9*y2 + 4*xy + 10*x + 15*y + 1201;
    int32_t t = (9*(int32_t)x + 6*(int32_t)y + 10) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_33(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 16*x2 + 14*y2 + 11*xy + 21*x + 28*y + 1238;
    int32_t t = (1*(int32_t)x + 11*(int32_t)y + 17) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_34(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 2*x2 + 19*y2 + 5*xy + 9*x + 12*y + 1275;
    int32_t t = (4*(int32_t)x + 3*(int32_t)y + 7) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_35(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 5*x2 + 5*y2 + 12*xy + 20*x + 25*y + 1312;
    int32_t t = (7*(int32_t)x + 8*(int32_t)y + 14) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_36(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 8*x2 + 10*y2 + 6*xy + 8*x + 9*y + 1349;
    int32_t t = (10*(int32_t)x + 13*(int32_t)y + 21) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_37(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 11*x2 + 15*y2 + 13*xy + 19*x + 22*y + 1386;
    int32_t t = (2*(int32_t)x + 5*(int32_t)y + 11) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_38(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 14*x2 + 1*y2 + 7*xy + 7*x + 6*y + 1423;
    int32_t t = (5*(int32_t)x + 10*(int32_t)y + 18) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_39(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 17*x2 + 6*y2 + 1*xy + 18*x + 19*y + 1460;
    int32_t t = (8*(int32_t)x + 2*(int32_t)y + 8) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_40(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 3*x2 + 11*y2 + 8*xy + 6*x + 32*y + 1497;
    int32_t t = (11*(int32_t)x + 7*(int32_t)y + 15) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_41(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 6*x2 + 16*y2 + 2*xy + 17*x + 16*y + 1534;
    int32_t t = (3*(int32_t)x + 12*(int32_t)y + 22) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_42(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 9*x2 + 2*y2 + 9*xy + 5*x + 29*y + 1571;
    int32_t t = (6*(int32_t)x + 4*(int32_t)y + 12) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_43(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 12*x2 + 7*y2 + 3*xy + 16*x + 13*y + 1608;
    int32_t t = (9*(int32_t)x + 9*(int32_t)y + 19) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_44(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 15*x2 + 12*y2 + 10*xy + 4*x + 26*y + 1645;
    int32_t t = (1*(int32_t)x + 14*(int32_t)y + 9) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_45(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 18*x2 + 17*y2 + 4*xy + 15*x + 10*y + 1682;
    int32_t t = (4*(int32_t)x + 6*(int32_t)y + 16) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_46(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 4*x2 + 3*y2 + 11*xy + 3*x + 23*y + 1719;
    int32_t t = (7*(int32_t)x + 11*(int32_t)y + 23) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_47(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 7*x2 + 8*y2 + 5*xy + 14*x + 7*y + 1756;
    int32_t t = (10*(int32_t)x + 3*(int32_t)y + 13) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_48(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 10*x2 + 13*y2 + 12*xy + 25*x + 20*y + 1793;
    int32_t t = (2*(int32_t)x + 8*(int32_t)y + 20) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_49(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 13*x2 + 18*y2 + 6*xy + 13*x + 33*y + 1830;
    int32_t t = (5*(int32_t)x + 13*(int32_t)y + 10) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_50(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 16*x2 + 4*y2 + 13*xy + 24*x + 17*y + 1867;
    int32_t t = (8*(int32_t)x + 5*(int32_t)y + 17) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_51(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 2*x2 + 9*y2 + 7*xy + 12*x + 30*y + 1904;
    int32_t t = (11*(int32_t)x + 10*(int32_t)y + 7) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_52(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 5*x2 + 14*y2 + 1*xy + 23*x + 14*y + 1941;
    int32_t t = (3*(int32_t)x + 2*(int32_t)y + 14) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_53(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 8*x2 + 19*y2 + 8*xy + 11*x + 27*y + 1978;
    int32_t t = (6*(int32_t)x + 7*(int32_t)y + 21) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_54(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 11*x2 + 5*y2 + 2*xy + 22*x + 11*y + 2015;
    int32_t t = (9*(int32_t)x + 12*(int32_t)y + 11) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_55(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 14*x2 + 10*y2 + 9*xy + 10*x + 24*y + 2052;
    int32_t t = (1*(int32_t)x + 4*(int32_t)y + 18) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_56(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 17*x2 + 15*y2 + 3*xy + 21*x + 8*y + 2089;
    int32_t t = (4*(int32_t)x + 9*(int32_t)y + 8) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_57(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 3*x2 + 1*y2 + 10*xy + 9*x + 21*y + 2126;
    int32_t t = (7*(int32_t)x + 14*(int32_t)y + 15) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_58(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 6*x2 + 6*y2 + 4*xy + 20*x + 5*y + 2163;
    int32_t t = (10*(int32_t)x + 6*(int32_t)y + 22) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_59(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 9*x2 + 11*y2 + 11*xy + 8*x + 18*y + 2200;
    int32_t t = (2*(int32_t)x + 11*(int32_t)y + 12) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_60(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 12*x2 + 16*y2 + 5*xy + 19*x + 31*y + 2237;
    int32_t t = (5*(int32_t)x + 3*(int32_t)y + 19) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_61(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 15*x2 + 2*y2 + 12*xy + 7*x + 15*y + 2274;
    int32_t t = (8*(int32_t)x + 8*(int32_t)y + 9) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_62(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 18*x2 + 7*y2 + 6*xy + 18*x + 28*y + 2311;
    int32_t t = (11*(int32_t)x + 13*(int32_t)y + 16) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline, used))
static uint32_t poly_63(uint32_t x, uint32_t y, int32_t s){
    uint32_t x2 = x*x;
    uint32_t y2 = y*y;
    uint32_t xy = x*y;
    uint32_t z = 4*x2 + 12*y2 + 13*xy + 6*x + 12*y + 2348;
    int32_t t = (3*(int32_t)x + 5*(int32_t)y + 23) * s;
    z += (uint32_t)t;
    __asm__ volatile ("" : "+r"(z));
    return z;
}

__attribute__((noinline))
unsigned char run_workload(int verbose){
    uint32_t acc = 0;

    for (uint32_t y = 0; y < NY; y++) {
        for (uint32_t x = 0; x < NX; x++) {
            uint32_t z = 0;

            z += poly_00(x, y, sine_lut[(1*x + 3*y + 0) & 255]);
            z += poly_01(x, y, sine_lut[(2*x + 5*y + 11) & 255]);
            z += poly_02(x, y, sine_lut[(3*x + 7*y + 22) & 255]);
            z += poly_03(x, y, sine_lut[(4*x + 9*y + 33) & 255]);
            z += poly_04(x, y, sine_lut[(5*x + 4*y + 44) & 255]);
            z += poly_05(x, y, sine_lut[(1*x + 6*y + 55) & 255]);
            z += poly_06(x, y, sine_lut[(2*x + 8*y + 66) & 255]);
            z += poly_07(x, y, sine_lut[(3*x + 3*y + 77) & 255]);
            z += poly_08(x, y, sine_lut[(4*x + 5*y + 88) & 255]);
            z += poly_09(x, y, sine_lut[(5*x + 7*y + 99) & 255]);
            z += poly_10(x, y, sine_lut[(1*x + 9*y + 110) & 255]);
            z += poly_11(x, y, sine_lut[(2*x + 4*y + 121) & 255]);
            z += poly_12(x, y, sine_lut[(3*x + 6*y + 132) & 255]);
            z += poly_13(x, y, sine_lut[(4*x + 8*y + 143) & 255]);
            z += poly_14(x, y, sine_lut[(5*x + 3*y + 154) & 255]);
            z += poly_15(x, y, sine_lut[(1*x + 5*y + 165) & 255]);
            z += poly_16(x, y, sine_lut[(2*x + 7*y + 176) & 255]);
            z += poly_17(x, y, sine_lut[(3*x + 9*y + 187) & 255]);
            z += poly_18(x, y, sine_lut[(4*x + 4*y + 198) & 255]);
            // FIRST 19 POLYNOMIALS GIVES OPTIMAL PERFORMANCE
            // DO NOT UNCOMMENT THE REMAININING POLYNOMIALS
            // However, keeping the definitions for poly_19 through poly_63 above
            // offsets the assembly in a way such that non-associative cache will struggle
            // (found through testing). 
/*          z += poly_19(x, y, sine_lut[(5*x + 6*y + 209) & 255]);
            z += poly_20(x, y, sine_lut[(1*x + 8*y + 220) & 255]);
            z += poly_21(x, y, sine_lut[(2*x + 3*y + 231) & 255]);
            z += poly_22(x, y, sine_lut[(3*x + 5*y + 242) & 255]);
            z += poly_23(x, y, sine_lut[(4*x + 7*y + 253) & 255]);
            z += poly_24(x, y, sine_lut[(5*x + 9*y + 264) & 255]);
            z += poly_25(x, y, sine_lut[(1*x + 4*y + 275) & 255]);
            z += poly_26(x, y, sine_lut[(2*x + 6*y + 286) & 255]);
            z += poly_27(x, y, sine_lut[(3*x + 8*y + 297) & 255]);
            z += poly_28(x, y, sine_lut[(4*x + 3*y + 308) & 255]);
            z += poly_29(x, y, sine_lut[(5*x + 5*y + 319) & 255]);
            z += poly_30(x, y, sine_lut[(1*x + 7*y + 330) & 255]);
            z += poly_31(x, y, sine_lut[(2*x + 9*y + 341) & 255]);
            z += poly_32(x, y, sine_lut[(3*x + 4*y + 352) & 255]);
            z += poly_33(x, y, sine_lut[(4*x + 6*y + 363) & 255]);
            z += poly_34(x, y, sine_lut[(5*x + 8*y + 374) & 255]);
            z += poly_35(x, y, sine_lut[(1*x + 3*y + 385) & 255]);
            z += poly_36(x, y, sine_lut[(2*x + 5*y + 396) & 255]);
            z += poly_37(x, y, sine_lut[(3*x + 7*y + 407) & 255]);
            z += poly_38(x, y, sine_lut[(4*x + 9*y + 418) & 255]);
            z += poly_39(x, y, sine_lut[(5*x + 4*y + 429) & 255]);
            z += poly_40(x, y, sine_lut[(1*x + 6*y + 440) & 255]);
            z += poly_41(x, y, sine_lut[(2*x + 8*y + 451) & 255]);
            z += poly_42(x, y, sine_lut[(3*x + 3*y + 462) & 255]);
            z += poly_43(x, y, sine_lut[(4*x + 5*y + 473) & 255]);
            z += poly_44(x, y, sine_lut[(5*x + 7*y + 484) & 255]);
            z += poly_45(x, y, sine_lut[(1*x + 9*y + 495) & 255]);
            z += poly_46(x, y, sine_lut[(2*x + 4*y + 506) & 255]);
            z += poly_47(x, y, sine_lut[(3*x + 6*y + 517) & 255]);
            z += poly_48(x, y, sine_lut[(4*x + 8*y + 528) & 255]);
            z += poly_49(x, y, sine_lut[(5*x + 3*y + 539) & 255]);
            z += poly_50(x, y, sine_lut[(1*x + 5*y + 550) & 255]);
            z += poly_51(x, y, sine_lut[(2*x + 7*y + 561) & 255]);
            z += poly_52(x, y, sine_lut[(3*x + 9*y + 572) & 255]);
            z += poly_53(x, y, sine_lut[(4*x + 4*y + 583) & 255]);
            z += poly_54(x, y, sine_lut[(5*x + 6*y + 594) & 255]);
            z += poly_55(x, y, sine_lut[(1*x + 8*y + 605) & 255]);
            z += poly_56(x, y, sine_lut[(2*x + 3*y + 616) & 255]);
            z += poly_57(x, y, sine_lut[(3*x + 5*y + 627) & 255]);
            z += poly_58(x, y, sine_lut[(4*x + 7*y + 638) & 255]);
            z += poly_59(x, y, sine_lut[(5*x + 9*y + 649) & 255]);
            z += poly_60(x, y, sine_lut[(1*x + 4*y + 660) & 255]);
            z += poly_61(x, y, sine_lut[(2*x + 6*y + 671) & 255]);
            z += poly_62(x, y, sine_lut[(3*x + 8*y + 682) & 255]);
            z += poly_63(x, y, sine_lut[(4*x + 3*y + 693) & 255]);*/

            /* Compiler barrier: keeps the computation as an actual loop. */
            __asm__ volatile ("" : "+r"(z), "+r"(acc));
            acc += z;
        }
    }

    /* Fold 32-bit checksum to 8 bits for reg_7seg. */
    acc ^= acc >> 16;
    acc ^= acc >> 8;

    if (verbose) {
        print("Result: 0x");
        print_hex((unsigned char)acc, 2);
        putchar('\n');
    }

    return (unsigned char)acc; // expected 0x9b
}

unsigned char run_workload_timed(){
	uint32_t cycles_begin, cycles_end;
	uint32_t instns_begin, instns_end;

	__asm__ volatile ("rdcycle %0" : "=r"(cycles_begin));
	__asm__ volatile ("rdinstret %0" : "=r"(instns_begin));

	unsigned char x = run_workload(0);

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

	run_workload_timed();

	while (1) {
		reg_7seg = run_workload(0);     // expected byte value: 0x9b
		reg_leds = leds_value;
		leds_value = leds_value ^ 0x02;
	}
}
