/*
 * kalman_steady_state.c — Steady-state (constant-gain) Kalman filter benchmark
 *
 * This is a SIMPLIFIED Kalman filter: a steady-state / constant-gain variant.
 * For a time-invariant system (constant F, H, Q, R) the covariance P and the
 * Kalman gain K converge to constant values, so they are precomputed offline
 * and the online loop runs only the predict / innovation / update with a FIXED
 * gain. This is a real, widely-used embedded technique — it avoids the
 * expensive covariance propagation and matrix inversion of a full filter.
 *
 * Computed per step (fixed gain K):
 *   predict     x_pred = F * x
 *   innovation  innov  = z - H * x_pred
 *   update      x      = x_pred + K * innov
 *
 * OMITTED relative to a full Kalman filter (see kalman_full.c):
 *   - covariance predict   P = F P F^T + Q
 *   - innovation covariance S = H P H^T + R
 *   - gain computation      K = P H^T S^-1   (requires a matrix inverse)
 *   - covariance update     P = (I - K H) P
 * Note: the hardcoded K here is an illustrative tuned gain, not the exact
 * solution of the discrete algebraic Riccati equation for this F/H/Q/R.
 *
 * 4-state model: position and velocity in 2D (px, py, vx, vy).
 * 2 observations: noisy position measurements.
 * Fixed-point Q4 arithmetic (1.0 represented as 16). 100 filter steps.
 *
 * Cache properties:
 *   Instruction cache  — tight triple-nested matrix-vector loops; large enough
 *                        footprint to benefit from set-associativity + wide lines.
 *   Data cache         — kf_F, kf_H, kf_K, kf_z_step, kf_z_noise are static
 *                        const in flash (.rodata), 264 bytes, read every step.
 *
 * Expected return value: 0xC7 (199 decimal) — estimated px in real units after
 * 100 steps tracking an object at velocity (vx=2, vy=1) units/step. Verified
 * against a host-PC run of the identical fixed-point code.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef ICEBREAKER
#  define MEM_TOTAL 0x20000
#else
#  error "Set -DICEBREAKER when compiling this C source file"
#endif

extern uint32_t sram;

#define reg_spictrl     (*(volatile uint32_t*)0x02000000)
#define reg_uart_clkdiv (*(volatile uint32_t*)0x02000004)
#define reg_uart_data   (*(volatile uint32_t*)0x02000008)
#define reg_leds        (*(volatile uint8_t*)0x03000000)
#define reg_7seg        (*(volatile uint8_t*)0x03000001)

extern uint32_t flashio_worker_begin;
extern uint32_t flashio_worker_end;

void flashio(uint8_t* data, int len, uint8_t wrencmd) {
	uint32_t func[&flashio_worker_end - &flashio_worker_begin];
	uint32_t* src_ptr = &flashio_worker_begin;
	uint32_t* dst_ptr = func;
	while (src_ptr != &flashio_worker_end)
		*(dst_ptr++) = *(src_ptr++);
	((void(*)(uint8_t*, uint32_t, uint32_t))func)(data, len, wrencmd);
}

void set_flash_qspi_flag() {
	uint8_t buffer[8];
	buffer[0] = 0x35; buffer[1] = 0x00;
	flashio(buffer, 2, 0);
	uint8_t sr2 = buffer[1];
	buffer[0] = 0x31; buffer[1] = sr2 | 2;
	flashio(buffer, 2, 0x50);
}
void set_flash_mode_qddr() { reg_spictrl = (reg_spictrl & ~0x007f0000) | 0x00670000; }

void putchar(char c) { if (c == '\n') putchar('\r'); reg_uart_data = c; }
void print(const char* p) { while (*p) putchar(*(p++)); }
void print_hex(uint32_t v, int digits) {
	for (int i = 7; i >= 0; i--) {
		char c = "0123456789abcdef"[(v >> (4*i)) & 15];
		if (c == '0' && i >= digits) continue;
		putchar(c); digits = i;
	}
}

void setup_picosoc(void) {
	reg_uart_clkdiv = 104; reg_leds = 0x00;
	set_flash_qspi_flag(); set_flash_mode_qddr();
}

// --------------------------------------------------------

#define KF_N_STATE  4
#define KF_N_OBS    2
#define KF_N_STEPS  100
#define KF_QSCALE   4

static const int32_t kf_F[KF_N_STATE][KF_N_STATE] = {
    {16,  0, 16,  0},
    { 0, 16,  0, 16},
    { 0,  0, 16,  0},
    { 0,  0,  0, 16},
};

static const int32_t kf_H[KF_N_OBS][KF_N_STATE] = {
    {16,  0,  0,  0},
    { 0, 16,  0,  0},
};

// Fixed (precomputed) steady-state gain — illustrative tuned values
static const int32_t kf_K[KF_N_STATE][KF_N_OBS] = {
    { 8,  0},
    { 0,  8},
    { 4,  0},
    { 0,  4},
};

static const int32_t kf_z_step[KF_N_OBS] = {32, 16};

static const int32_t kf_z_noise[16][KF_N_OBS] = {
    { 3,  1}, {-3, -1}, { 2,  4}, {-2, -4},
    { 5, -2}, {-5,  2}, { 1, -3}, {-1,  3},
    { 4,  0}, {-4,  0}, { 2,  3}, {-2, -3},
    { 0,  2}, { 1, -1}, {-1,  1}, { 0, -2},
};

unsigned char run_workload(void) {
    int32_t x[KF_N_STATE] = {0, 0, 32, 16};
    int32_t z[KF_N_OBS]   = {-3, 1};

    for (int t = 0; t < KF_N_STEPS; t++) {
        z[0] += kf_z_step[0] + kf_z_noise[t & 15][0];
        z[1] += kf_z_step[1] + kf_z_noise[t & 15][1];

        int32_t x_pred[KF_N_STATE];
        for (int i = 0; i < KF_N_STATE; i++) {
            int32_t acc = 0;
            for (int j = 0; j < KF_N_STATE; j++)
                acc += kf_F[i][j] * x[j];
            x_pred[i] = acc >> KF_QSCALE;
        }

        int32_t innov[KF_N_OBS];
        for (int i = 0; i < KF_N_OBS; i++) {
            int32_t hx = 0;
            for (int j = 0; j < KF_N_STATE; j++)
                hx += kf_H[i][j] * x_pred[j];
            innov[i] = z[i] - (hx >> KF_QSCALE);
        }

        for (int i = 0; i < KF_N_STATE; i++) {
            int32_t ki = 0;
            for (int j = 0; j < KF_N_OBS; j++)
                ki += kf_K[i][j] * innov[j];
            x[i] = x_pred[i] + (ki >> KF_QSCALE);
        }
    }

    return (unsigned char)((x[0] >> KF_QSCALE) & 0xFF);
}

void main(void) {
	setup_picosoc();
	unsigned char leds_value = 0x02;
	while (1) { reg_7seg = run_workload(); reg_leds = leds_value; leds_value ^= 0x02; }
}
