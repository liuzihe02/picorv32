#include <stdint.h>
#include <stdbool.h>

#ifdef ICEBREAKER
#  define MEM_TOTAL 0x20000 /* 128 KB */
#else
#  error "Set -DICEBREAKER when compiling this C source file"
#endif

extern uint32_t sram;

#define reg_spictrl (*(volatile uint32_t*)0x02000000)
#define reg_uart_clkdiv (*(volatile uint32_t*)0x02000004)
#define reg_uart_data (*(volatile uint32_t*)0x02000008)
#define reg_leds (*(volatile uint8_t*)0x03000000)
#define reg_7seg (*(volatile uint8_t*)0x03000001)

extern uint32_t flashio_worker_begin;
extern uint32_t flashio_worker_end;

void* memcpy(void* aa, const void* bb, long n)
{
	char* a = aa;
	const char* b = bb;
	while (n--)
		*(a++) = *(b++);
	return aa;
}

void flashio(uint8_t* data, int len, uint8_t wrencmd)
{
	uint32_t func[&flashio_worker_end - &flashio_worker_begin];
	uint32_t* src_ptr = &flashio_worker_begin;
	uint32_t* dst_ptr = func;

	while (src_ptr != &flashio_worker_end)
		*(dst_ptr++) = *(src_ptr++);

	((void(*)(uint8_t*, uint32_t, uint32_t))func)(data, len, wrencmd);
}

void set_flash_qspi_flag(void)
{
	uint8_t buffer[8];

	buffer[0] = 0x35;
	buffer[1] = 0x00;
	flashio(buffer, 2, 0);

	buffer[0] = 0x31;
	buffer[1] = buffer[1] | 2;
	flashio(buffer, 2, 0x50);
}

void set_flash_mode_qddr(void)
{
	reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00670000;
}

void setup_picosoc(void)
{
	reg_uart_clkdiv = 104;
	reg_7seg = 0x03;
	reg_leds = 0x00;
	set_flash_qspi_flag();
	set_flash_mode_qddr();
}

#define SORT_SIZE 100

static uint32_t benchmark_sort(void)
{
	uint8_t numbers[SORT_SIZE] = {
		142,  87, 213,  42, 119,   8, 176,  54, 231,  99,
		 12, 165,  74, 201,  33, 150,  88, 245,  19, 111,
		182,  63, 137,  95, 222,   4, 158,  81, 209,  47,
		126,  71, 194,  28, 147, 252,  91,  16, 115, 170,
		 58, 239,  83, 132,   2, 205,  67, 149, 226,  38,
		104, 188,  51, 161,  94, 242,  11, 123,  79, 217,
		134,  45, 173,  89, 250,  23, 155,  61, 199, 108,
		 31, 140, 212,  76,   7, 185,  53, 167, 234,  92,
		121,  14, 203,  69, 152,  41, 228,  85, 114, 191,
		 26, 179,  60, 247,  97, 136,   5, 221,  73, 162
	};
	uint32_t checksum = 0;

	for (int i = 0; i < SORT_SIZE - 1; i++) {
		for (int j = 0; j < SORT_SIZE - i - 1; j++) {
			if (numbers[j] > numbers[j + 1]) {
				uint8_t tmp = numbers[j];
				numbers[j] = numbers[j + 1];
				numbers[j + 1] = tmp;
			}
		}
	}

	for (int i = 0; i < SORT_SIZE; i++)
		checksum = (checksum << 5) ^ (checksum >> 2) ^ numbers[i];

	return checksum;
}

static uint32_t benchmark_crc(void)
{
	uint32_t crc = 0xffffffff;
	uint32_t value = 0x12345678;

	for (int i = 0; i < 4096; i++) {
		value ^= value << 13;
		value ^= value >> 17;
		value ^= value << 5;
		crc ^= value;

		for (int bit = 0; bit < 8; bit++) {
			uint32_t mask = 0 - (crc & 1);
			crc = (crc >> 1) ^ (0xedb88320 & mask);
		}
	}

	return ~crc;
}

static uint32_t soft_mul8(uint32_t a, uint32_t b)
{
	uint32_t r = 0;

	for (int i = 0; i < 8; i++) {
		if (b & 1)
			r += a;
		a <<= 1;
		b >>= 1;
	}

	return r;
}

static uint32_t benchmark_matrix(void)
{
	uint8_t a[8][8];
	uint8_t b[8][8];
	uint16_t c[8][8];
	uint32_t checksum = 0;

	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			a[y][x] = (uint8_t)((y << 4) + x + 3);
			b[y][x] = (uint8_t)((x << 3) ^ (y + 11));
			c[y][x] = 0;
		}
	}

	for (int repeat = 0; repeat < 16; repeat++) {
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				uint32_t acc = 0;
				for (int k = 0; k < 8; k++)
					acc += soft_mul8(a[y][k], b[k][x]);
				c[y][x] = (uint16_t)acc;
				checksum ^= (acc << ((x + y) & 7)) + repeat;
			}
		}
	}

	return checksum;
}

#define CACHE_PAD() __asm__ volatile (".rept 12\nnop\n.endr")

#define CACHE_BLOCK(n, a, b, c) \
static uint32_t __attribute__((noinline)) cache_block_##n(uint32_t x) \
{ \
	CACHE_PAD(); \
	x ^= (a); \
	x += (x << (b)) ^ (x >> (c)); \
	x ^= x >> 11; \
	CACHE_PAD(); \
	return x + (a); \
}

CACHE_BLOCK(00, 0x13579bdf, 3, 5)
CACHE_BLOCK(01, 0x2468ace1, 4, 7)
CACHE_BLOCK(02, 0x10203040, 5, 3)
CACHE_BLOCK(03, 0x89abcdef, 6, 9)
CACHE_BLOCK(04, 0x55aa00ff, 2, 6)
CACHE_BLOCK(05, 0x0f1e2d3c, 7, 4)
CACHE_BLOCK(06, 0xc001d00d, 3, 8)
CACHE_BLOCK(07, 0x31415926, 5, 6)
CACHE_BLOCK(08, 0x27182818, 4, 5)
CACHE_BLOCK(09, 0xa5a55a5a, 6, 7)
CACHE_BLOCK(10, 0xdeadbeef, 2, 9)
CACHE_BLOCK(11, 0x01020408, 7, 3)
CACHE_BLOCK(12, 0xf0e1d2c3, 3, 4)
CACHE_BLOCK(13, 0x76543210, 5, 8)
CACHE_BLOCK(14, 0xabcdef01, 6, 5)
CACHE_BLOCK(15, 0x600dcafe, 4, 6)

static uint32_t benchmark_footprint(void)
{
	uint32_t x = 0x12345678;

	for (int i = 0; i < 128; i++) {
		x = cache_block_00(x);
		x = cache_block_01(x);
		x = cache_block_02(x);
		x = cache_block_03(x);
		x = cache_block_04(x);
		x = cache_block_05(x);
		x = cache_block_06(x);
		x = cache_block_07(x);
		x = cache_block_08(x);
		x = cache_block_09(x);
		x = cache_block_10(x);
		x = cache_block_11(x);
		x = cache_block_12(x);
		x = cache_block_13(x);
		x = cache_block_14(x);
		x = cache_block_15(x);
	}

	return x;
}

static uint32_t benchmark_mixed(void)
{
	uint32_t r = 0;

	r ^= benchmark_sort();
	r ^= benchmark_crc();
	r ^= benchmark_matrix();
	r ^= benchmark_footprint();

	return r;
}

unsigned char run_workload(void)
{
	uint32_t x = benchmark_mixed();

	return (unsigned char)(x ^ (x >> 8) ^ (x >> 16) ^ (x >> 24));
}

void main(void)
{
	setup_picosoc();

	unsigned char leds_value = 0x02;

	while (1) {
		reg_7seg = run_workload();
		reg_leds = leds_value;
		leds_value = leds_value ^ 0x02;
	}

}
