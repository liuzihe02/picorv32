#include <stdint.h>

#define reg_leds (*(volatile uint8_t*)0x03000000)
#define reg_7seg (*(volatile uint8_t*)0x03000001)
#define reg_spictrl (*(volatile uint32_t*)0x02000000)

//#define USE_MACC

//use any matrix hardware otherwise normal multiplication
static inline int fast_mul(int a, int b) {
#ifdef USE_MACC
    int result;
    __asm__ volatile (
        "mv a0, %1\n"
        "mv a1, %2\n"
        ".word 0x00B5052B\n"
        "mv %0, a0\n"
        : "=r"(result) : "r"(a), "r"(b) : "a0", "a1"
    );
    return result;
#else
    return a * b;
#endif
}

#define TAPS    16
#define SAMPLES 64

//triangle wave data for filter coefficients
static const int coeffs[TAPS] = {
    1, 2, 3, 4, 5, 6, 7, 8,
    8, 7, 6, 5, 4, 3, 2, 1
};
//data for FIR filter (inputs)
static const int signal[SAMPLES + TAPS] = {
    14, 21, 28, 35, 42, 49, 56, 63,
    70, 77, 84, 91, 98,  5, 12, 19,
    26, 33, 40, 47, 54, 61, 68, 75,
    82, 89, 96,  3, 10, 17, 24, 31,
    38, 45, 52, 59, 66, 73, 80, 87,
    94,  1,  8, 15, 22, 29, 36, 43,
    50, 57, 64, 71, 78, 85, 92, 99,
     6, 13, 20, 27, 34, 41, 48, 55,
    62, 69, 76, 83, 90, 97,  4, 11,
    18, 25, 32, 39, 46, 53, 60, 67,
};


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

void setup_picosoc(void){
	//reg_uart_clkdiv = 104; // Baud = 1152060
    reg_7seg = 0x07;       // represents Demo 02
	reg_leds = 0x00;
	set_flash_qspi_flag();
    set_flash_mode_qddr(); // fastest SPI flash
}

// --------------------------------------------------------
unsigned char run_workload(void) {
    int out[SAMPLES];
    int i, k;
//multiplies each input value by the coefficient and sums them up for fir filter
    for (i = 0; i < SAMPLES; i++) {
        int sum = 0;
        for (k = 0; k < TAPS; k++)
            sum += fast_mul(coeffs[k], signal[i + k]);
        out[i] = sum >> 7;
    }
//xor to give one single output
    unsigned char res = 0;
    for (i = 0; i < SAMPLES; i++)
        res ^= (unsigned char)(out[i] & 0xFF);

    return res;  // expect0x05
}

void main(void) {
    setup_picosoc();
    unsigned char leds_value = 0x02;
    while (1) {
        reg_7seg = run_workload();
        reg_leds = leds_value;
        leds_value = leds_value ^ 0x02; //toggle per iteration
    }
}
