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
	reg_uart_clkdiv = 104; // Baud = 1152060
    reg_7seg = 0x02;       // represents Demo 02
	reg_leds = 0x00;
	set_flash_qspi_flag();
    set_flash_mode_spi(); // fastest SPI flash
}



#define cordic_1K 0x26DD3B6A
#define half_pi 0x6487ED51
//#define MUL 1073741824.000000
#define CORDIC_NTAB 32

const int cordic_ctab [] = {0x3243F6A8, 0x1DAC6705, 0x0FADBAFC, 0x07F56EA6, 0x03FEAB76, 0x01FFD55B, 0x00FFFAAA, 0x007FFF55, 0x003FFFEA, 0x001FFFFD, 0x000FFFFF, 0x0007FFFF, 0x0003FFFF, 0x0001FFFF, 0x0000FFFF, 0x00007FFF, 0x00003FFF, 0x00001FFF, 0x00000FFF, 0x000007FF, 0x000003FF, 0x000001FF, 0x000000FF, 0x0000007F, 0x0000003F, 0x0000001F, 0x0000000F, 0x00000008, 0x00000004, 0x00000002, 0x00000001, 0x00000000, };

void cordic(int theta, int *s, int *c, int n){
    int k, d, tx, ty, tz;
    int x = cordic_1K, y = 0, z = theta;
    n = (n > CORDIC_NTAB) ? CORDIC_NTAB : n;
    for (k = 0; k < n; ++k){
        d = z>>31;
        //get sign. for other architectures, you might want to use the more portable version
        //d = z>=0 ? 0 : -1;
        tx = x - (((y>>k) ^ d) - d);
        ty = y + (((x>>k) ^ d) - d);
        tz = z - ((cordic_ctab[k] ^ d) - d);
        x = tx; y = ty; z = tz;
    }  
    *c = x; *s = y;
}


#define ARRAY_SIZE 100

const int aa[] = {0x0, 0x1015bf9, 0x202b7f2, 0x30413eb, 0x4056fe4, 0x506cbdd, 0x60827d6, 0x70983cf, 0x80adfc9, 0x90c3bc2,
0xa0d97bb, 0xb0ef3b4, 0xc104fad, 0xd11aba6, 0xe13079f, 0xf146398, 0x1015bf92, 0x11171b8b, 0x12187784, 0x1319d37d,
0x141b2f76, 0x151c8b6f, 0x161de768, 0x171f4362, 0x18209f5b, 0x1921fb54, 0x1a23574d, 0x1b24b346, 0x1c260f3f, 0x1d276b38,
0x1e28c731, 0x1f2a232b, 0x202b7f24, 0x212cdb1d, 0x222e3716, 0x232f930f, 0x2430ef08, 0x25324b01, 0x2633a6fa, 0x273502f4,
0x28365eed, 0x2937bae6, 0x2a3916df, 0x2b3a72d8, 0x2c3bced1, 0x2d3d2aca, 0x2e3e86c4, 0x2f3fe2bd, 0x30413eb6, 0x31429aaf,
0x3243f6a8, 0x334552a1, 0x3446ae9a, 0x35480a93, 0x3649668d, 0x374ac286, 0x384c1e7f, 0x394d7a78, 0x3a4ed671, 0x3b50326a,
0x3c518e63, 0x3d52ea5c, 0x3e544656, 0x3f55a24f, 0x4056fe48, 0x41585a41, 0x4259b63a, 0x435b1233, 0x445c6e2c, 0x455dca26,
0x465f261f, 0x47608218, 0x4861de11, 0x49633a0a, 0x4a649603, 0x4b65f1fc, 0x4c674df5, 0x4d68a9ef, 0x4e6a05e8, 0x4f6b61e1,
0x506cbdda, 0x516e19d3, 0x526f75cc, 0x5370d1c5, 0x54722dbe, 0x557389b8, 0x5674e5b1, 0x577641aa, 0x58779da3, 0x5978f99c,
0x5a7a5595, 0x5b7bb18e, 0x5c7d0d88, 0x5d7e6981, 0x5e7fc57a, 0x5f812173, 0x60827d6c, 0x6183d965, 0x6285355e, 0x63869157};

unsigned char run_workload(int verbose){
    int s, c;
    int i;    
    for(i = 0; i < ARRAY_SIZE; i++){
        cordic(aa[i], &s, &c, 32);
        cordic(aa[i], &s, &c, 32);
        cordic(aa[i], &s, &c, 32);
        cordic(aa[i], &s, &c, 32);
        cordic(aa[i], &s, &c, 32);
    }

    if (verbose){
        print("0x");
        print_hex(s, 8); // expects 0x3ffdfa8e
        putchar('\n');
    }

    return (s & 0xFF); // 0x8E
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

    run_workload(1); // for debug 
    run_workload_timed(); // for the first time, CPI measurement
                          
    while (1) {
        // calculation that produces a unique answer
        reg_7seg = run_workload(0);     // 7-segment display
        reg_leds = leds_value;
        leds_value = leds_value ^ 0x02; // toggle LED1
    }
}
