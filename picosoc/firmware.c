/*
 *  PicoSoC - A simple example SoC using PicoRV32
 *
 *  Copyright (C) 2017  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <stdint.h>
#include <stdbool.h>
#include "benchmarks.h"

#ifdef ICEBREAKER
#  define MEM_TOTAL 0x20000 /* 128 KB */
#elif HX8KDEMO
#  define MEM_TOTAL 0x200 /* 2 KB */
#else
#  error "Set -DICEBREAKER or -DHX8KDEMO when compiling firmware.c"
#endif

// a pointer to this is a null pointer, but the compiler does not
// know that because "sram" is a linker symbol from sections.lds.
extern uint32_t sram;

#define reg_spictrl (*(volatile uint32_t*)0x02000000)
#define reg_uart_clkdiv (*(volatile uint32_t*)0x02000004)
#define reg_uart_data (*(volatile uint32_t*)0x02000008)
// GPIO at 0x03000000
// [7:0] = LEDs, [15:8] = 7-segment display
#define reg_leds (*(volatile uint32_t*)0x03000000)
#define reg_7seg (*(volatile uint8_t*)0x03000001)

/* =====================================================================
 * ENGINEERING LOG — I-cache freeze
 * ---------------------------------------------------------------------
 * Symptom: with the I-cache added, the board froze intermittently when a key was pressed during the all-LEDs-on (FF) phase; the 00 phase was fine.
 *
 * Root cause: the cache immortalises a *transient* flash-read error. Without a cache a bad word self-heals (re-fetched next loop)
 * cached, it's stored with a valid tag and replayed forever -> CPU runs a corrupt instr and hangs.
 *
 * Trigger: simultaneous output switching. The keypress write reg_leds=0 flipped all ~15 driven pins 0xFF->0x00 at once (large current change) just before a flash fetch, corrupting the sampled word.
 * A powered USB hub did NOT help -> it's on-chip switching noise / ground bounce, not supply margin.
 * Pressing during the 00 phase is a no-op write -> no glitch.
 *
 * Fix (symptom-level): boot() and getchar_prompt() drive ONE LED at a time, never the all-on word.
 * ===================================================================== */

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


// Flash read-mode setters (write reg_spictrl); used by run_flashmodes() for
// measurement, not on the scored path.
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

// --------------------------------------------------------

void putchar(char c)
{
	if (c == '\n')
		putchar('\r');
	reg_uart_data = c;
}

void print(const char *p)
{
	while (*p)
		putchar(*(p++));
}

void print_hex(uint32_t v, int digits)
{
	for (int i = 7; i >= 0; i--) {
		char c = "0123456789abcdef"[(v >> (4*i)) & 15];
		if (c == '0' && i >= digits) continue;
		putchar(c);
		digits = i;
	}
}

// Print uint32_t as decimal. Original was a hardcoded if-else chain capped at 999,
// because -nostdlib means no libgcc (no __udivsi3). With -march=rv32im the compiler
// emits hardware div/rem instructions directly, so modulo just works.
void print_dec(uint32_t v)
{
	char buf[10];
	int i = 0;
	if (v == 0) { putchar('0'); return; }
	while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
	while (i > 0) putchar(buf[--i]);
}

char getchar_prompt(char *prompt)
{
	int32_t c = -1;

	uint32_t cycles_begin, cycles_now, cycles;
	__asm__ volatile ("rdcycle %0" : "=r"(cycles_begin));

	// Heartbeat on LED1 only (0x02), never the all-on word (~0). Driving all 7 LEDs + 7-seg caused a simultaneous-switching transient on keypress that the I-cache turned into a permanent freeze.
	reg_leds = 0x02;

	if (prompt)
		print(prompt);

	while (c == -1) {
		__asm__ volatile ("rdcycle %0" : "=r"(cycles_now));
		cycles = cycles_now - cycles_begin;
		if (cycles > 12000000) {
			if (prompt)
				print(prompt);
			cycles_begin = cycles_now;
			reg_leds = reg_leds ^ 0x02;   // toggle LED1 only
		}
		c = reg_uart_data;
	}

	reg_leds = 0;
	return c;
}

char getchar()
{
	return getchar_prompt(0);
}

void cmd_print_spi_state()
{
	print("SPI State:\n");

	print("  LATENCY ");
	print_dec((reg_spictrl >> 16) & 15);
	print("\n");

	print("  DDR ");
	if ((reg_spictrl & (1 << 22)) != 0)
		print("ON\n");
	else
		print("OFF\n");

	print("  QSPI ");
	if ((reg_spictrl & (1 << 21)) != 0)
		print("ON\n");
	else
		print("OFF\n");

	print("  CRM ");
	if ((reg_spictrl & (1 << 20)) != 0)
		print("ON\n");
	else
		print("OFF\n");
}

uint32_t xorshift32(uint32_t *state)
{
	/* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;

	return x;
}

void cmd_memtest()
{
	int cyc_count = 5;
	int stride = 256;
	uint32_t state;

	volatile uint32_t *base_word = (uint32_t *) 0;
	volatile uint8_t *base_byte = (uint8_t *) 0;

	print("Running memtest ");

	// Walk in stride increments, word access
	for (int i = 1; i <= cyc_count; i++) {
		state = i;

		for (int word = 0; word < MEM_TOTAL / sizeof(int); word += stride) {
			*(base_word + word) = xorshift32(&state);
		}

		state = i;

		for (int word = 0; word < MEM_TOTAL / sizeof(int); word += stride) {
			if (*(base_word + word) != xorshift32(&state)) {
				print(" ***FAILED WORD*** at ");
				print_hex(4*word, 4);
				print("\n");
				return;
			}
		}

		print(".");
	}

	// Byte access
	for (int byte = 0; byte < 128; byte++) {
		*(base_byte + byte) = (uint8_t) byte;
	}

	for (int byte = 0; byte < 128; byte++) {
		if (*(base_byte + byte) != (uint8_t) byte) {
			print(" ***FAILED BYTE*** at ");
			print_hex(byte, 4);
			print("\n");
			return;
		}
	}

	print(" passed\n");
}

// --------------------------------------------------------

void cmd_read_flash_id()
{
	uint8_t buffer[17] = { 0x9F, /* zeros */ };
	flashio(buffer, 17, 0);

	for (int i = 1; i <= 16; i++) {
		putchar(' ');
		print_hex(buffer[i], 2);
	}
	putchar('\n');
}

// --------------------------------------------------------

uint8_t cmd_read_flash_reg(uint8_t cmd)
{
	uint8_t buffer[2] = {cmd, 0};
	flashio(buffer, 2, 0);
	return buffer[1];
}

void print_reg_bit(int val, const char *name)
{
	for (int i = 0; i < 12; i++) {
		if (*name == 0)
			putchar(' ');
		else
			putchar(*(name++));
	}

	putchar(val ? '1' : '0');
	putchar('\n');
}

void cmd_read_flash_regs()
{
	putchar('\n');

	uint8_t sr1 = cmd_read_flash_reg(0x05);
	uint8_t sr2 = cmd_read_flash_reg(0x35);
	uint8_t sr3 = cmd_read_flash_reg(0x15);

	print_reg_bit(sr1 & 0x01, "S0  (BUSY)");
	print_reg_bit(sr1 & 0x02, "S1  (WEL)");
	print_reg_bit(sr1 & 0x04, "S2  (BP0)");
	print_reg_bit(sr1 & 0x08, "S3  (BP1)");
	print_reg_bit(sr1 & 0x10, "S4  (BP2)");
	print_reg_bit(sr1 & 0x20, "S5  (TB)");
	print_reg_bit(sr1 & 0x40, "S6  (SEC)");
	print_reg_bit(sr1 & 0x80, "S7  (SRP)");
	putchar('\n');

	print_reg_bit(sr2 & 0x01, "S8  (SRL)");
	print_reg_bit(sr2 & 0x02, "S9  (QE)");
	print_reg_bit(sr2 & 0x04, "S10 ----");
	print_reg_bit(sr2 & 0x08, "S11 (LB1)");
	print_reg_bit(sr2 & 0x10, "S12 (LB2)");
	print_reg_bit(sr2 & 0x20, "S13 (LB3)");
	print_reg_bit(sr2 & 0x40, "S14 (CMP)");
	print_reg_bit(sr2 & 0x80, "S15 (SUS)");
	putchar('\n');

	print_reg_bit(sr3 & 0x01, "S16 ----");
	print_reg_bit(sr3 & 0x02, "S17 ----");
	print_reg_bit(sr3 & 0x04, "S18 (WPS)");
	print_reg_bit(sr3 & 0x08, "S19 ----");
	print_reg_bit(sr3 & 0x10, "S20 ----");
	print_reg_bit(sr3 & 0x20, "S21 (DRV0)");
	print_reg_bit(sr3 & 0x40, "S22 (DRV1)");
	print_reg_bit(sr3 & 0x80, "S23 (HOLD)");
	putchar('\n');
}

// --------------------------------------------------------

// The scored workload: the ONE benchmark wired to the LED1 scope loop
// (run_scope). Change this single line to pick which benchmark is scored.
unsigned char run_workload(void)
{
	return bench_matmul();
}

// Scope run: loop the workload forever, timing each pass and toggling LED1 for the Picoscope.
// Prints cycles / instrs / CPI over UART. Stops on a key.
void run_scope(void)
{
	uint8_t leds_val = 0x02;
	print("Scope run: workload on LED1 (press any key to stop)...\n");

	while (1) {
		if ((int32_t)reg_uart_data != -1) return;

		uint32_t ins;
		uint32_t cyc = time_benchmark(run_workload, &ins, 0);

		reg_leds = leds_val;
		leds_val ^= 0x02;

		print("cycles=");
		print_dec(cyc);
		print(" instrs=");
		print_dec(ins);

		print(" CPI=");
		print_cpi(cyc, ins);
		print(" ms=");
		print_ms(cyc);
		print("\n");
	}
}

// --------------------------------------------------------

void run_flashmodes(void)
{
	static const struct { const char *name; void (*set)(void); } modes[] = {
		{ "spi ", set_flash_mode_spi  },
		{ "dual", set_flash_mode_dual },
		{ "quad", set_flash_mode_quad },
		{ "qddr", set_flash_mode_qddr },
	};
	print("\nflash-mode sweep (bench_cold)   cycles   instrs\n");
	for (int m = 0; m < 4; m++) {
		modes[m].set();
		uint32_t ins;
		uint32_t cyc = time_benchmark(bench_cold, &ins, 0);
		print("  "); print(modes[m].name); print("  ");
		print_dec(cyc); print("  "); print_dec(ins); print("\n");
	}
	set_flash_mode_spi();   // restore single-SPI baseline
	print("done\n");
}

// --------------------------------------------------------

void cmd_echo()
{
	print("Return to menu by sending '!'\n\n");
	char c;
	while ((c = getchar()) != '!')
		putchar(c);
}

// --------------------------------------------------------

// Hardware boot sequence:
// QSPI flash mode (in original firmware), uart baud rate, clear led and 7seg outputs.
// LEDs show progress (1 -> 3 -> 7) matching start.s convention.
void boot()
{
	// Low-current boot progress: ONE LED per step (led1 -> led2 -> led3), never the all-on word.
	reg_leds = 0x02;            // led1
	reg_uart_clkdiv = (F_CLK_HZ + BAUD/2) / BAUD;   // rounded f_clk / baud (F_CLK_HZ, BAUD in benchmarks.h)
	reg_7seg = 0x00;

	reg_leds = 0x04;            // led2
	set_flash_qspi_flag();

	reg_leds = 0x08;            // led3
}

void main()
{
	boot();
	print("Booting..\n");

	while (getchar_prompt("Press ENTER to continue..\n") != '\r') { /* wait */ }

	print("\n");
	print("  ____  _          ____         ____\n");
	print(" |  _ \\(_) ___ ___/ ___|  ___  / ___|\n");
	print(" | |_) | |/ __/ _ \\___ \\ / _ \\| |\n");
	print(" |  __/| | (_| (_) |__) | (_) | |___\n");
	print(" |_|   |_|\\___\\___/____/ \\___/ \\____|\n");
	print("\n");

	print("Total memory: ");
	print_dec(MEM_TOTAL / 1024);
	print(" KiB\n");
	print("\n");

	//cmd_memtest(); // test overwrites bss and data memory
	print("\n");

	cmd_print_spi_state();
	print("\n");

	while (1)
	{
		print("\n");

		print("Select an action:\n");
		print("\n");
		print("   [1] Read SPI Flash ID\n");
		print("   [2] Read SPI Config Regs\n");
		print("   [M] Run Memtest\n");
		print("   [S] Print SPI state\n");
		print("   [B] Run scope (workload)\n");
		print("   [T] Run benchmark suite\n");
		print("   [F] Flash-mode sweep\n");
		print("   [e] Echo UART\n");
		print("\n");

		for (int rep = 10; rep > 0; rep--)
		{
			print("Command> ");
			char cmd = getchar();
			if (cmd > 32 && cmd < 127)
				putchar(cmd);
			print("\n");

			switch (cmd)
			{
			case '1':
				cmd_read_flash_id();
				break;
			case '2':
				cmd_read_flash_regs();
				break;
			case 'M':
				cmd_memtest();
				break;
			case 'S':
				cmd_print_spi_state();
				break;
			case 'B':
				run_scope();
				break;
			case 'T':
				run_benchmarks();
				break;
			case 'F':
				run_flashmodes();
				break;
			case 'e':
				cmd_echo();
				break;
			default:
				continue;
			}

			break;
		}
	}
}
