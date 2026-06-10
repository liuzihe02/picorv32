#include <stdint.h>
#include <stdbool.h>

#  define MEM_TOTAL 0x20000 /* 128 KB */

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

void *memset(void *aa, int c, long n) {
    char *a = aa;
    while (n--) *(a++) = c;
    return aa;
}

#endif

// --------------------------------------------------------

void setup_picosoc(void){
	reg_uart_clkdiv = 104; // Baud = 1152060
    reg_7seg = 0x00;       // represents GB3 2026
	reg_leds = 0x00;
	set_flash_qspi_flag();

}

#define MAT_SIZE 10
unsigned char run_workload() {

    int mat1[MAT_SIZE][MAT_SIZE];
    int mat2[MAT_SIZE][MAT_SIZE];

    int next = 0;

    // Populate matrices
    int i, j;
    for (i = 0; i < MAT_SIZE; i++) {
        for (j = 0; j < MAT_SIZE; j++) {
            mat1[i][j] = next;
            next++;
            mat2[i][j] = next;
            next++;
        }
    }

    // Calculate the multiplication
    int mat_result[MAT_SIZE][MAT_SIZE] = {0};
    int row_result[MAT_SIZE];
    int k;
    for (i = 0; i < MAT_SIZE; i++) {
        for (j = 0; j < MAT_SIZE; j++) {
            // Calculate the multiplication between one row and column
            for (k = 0; k < MAT_SIZE; k++) {
                row_result[k] = mat1[i][k] * mat2[k][j];
            }
            // Sum row result
            for (k = 0; k < MAT_SIZE; k++) {
                mat_result[i][j] += row_result[k];
            }
            
        }
    }

    // Calculate the sum of all elements
    int total_sum = 0;
    for (i = 0; i < MAT_SIZE; i++) {
        for (j = 0; j < MAT_SIZE; j++) {
            total_sum += mat_result[i][j];
        }
    }

    return total_sum & 0xFF; // Return the least significant byte of the total sum
}

void main()
{
    setup_picosoc();

    unsigned char leds_value = 0x02;
    while (1) {
        //reg_7seg = 0x00; // blank display
        reg_7seg = run_workload(); // display
        reg_leds = leds_value;
        leds_value = leds_value ^ 0x02; // toggle LED1
    }
}