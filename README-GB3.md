# GB3 RISC-V Project

This is a design document containing brief background and implementation plan for the GB3 RISC-V Project. For detailed technical background on microarchitecture and details of `picorv32`, please refer to the [technical deep dive](guide/guide.pdf) that Zach maintains.

> Zach reccomends using [markdown-preview-enhanced](https://shd101wyy.github.io/markdown-preview-enhanced/#/) to view md files.

## Core Files (For LLM context)

> Please have your coding agent read the below files (at least tier 1 files) first before asking it anything, if not it will probably do stupid shit.

This will almost always require a model with at least 500K token context window. Files are tiered by relevance to the committed optimisation plan (fetch-bound first → fast-read + I-cache, then core CPI, then pipeline; PLL free; coprocessors out).

Tier 1 Files

| File | Lines | Role |
| --- | ---: | --- |
| `picorv32.v` | 3049 | CPU core. Main module `picorv32`; also `picorv32_pcpi_mul/fast_mul/div`, `picorv32_regs`, `picorv32_axi`, `picorv32_wb`. |
| `picosoc/spimemio.v` | 579 | SPI flash controller — the dominant fetch-latency block: fast-read reset defaults, jump penalty, line-buffer / I-cache insertion point. |
| `picosoc/icebreaker.v` | 240 | Top-level for the iCEBreaker board: instantiates `picosoc` (iCE40UP5K config, SPRAM, 7-seg; no PLL, raw 12 MHz). Current rv32im params: `BARREL_SHIFTER=0`, `ENABLE_MUL=0`, `ENABLE_DIV=1`, `ENABLE_FAST_MUL=1`, `ENABLE_COMPRESSED=0`. |
| `picosoc/picosoc.v` | 262 | SoC wrapper. Wires CPU to SRAM, UART, SPI flash, GPIO. Address decode + memory map live here (the cache sits on this bus). |
| `README.md` | — | Full documentation of all configuration parameters. |
| `picosoc/README.md` | — | picosoc documentation. |

Ideally also get it to read the [technical deep dive](guide/guide.tex)

Tier 2 Files

| File | Lines | Role |
| --- | ---: | --- |
| `picosoc/sections.lds` | — | Linker script: `.text`/`.rodata` → FLASH (`0x00100000`), `.data`/`.bss`/`.heap` → SPRAM — the memory map (code in flash = the fetch-bound reality). Preprocessed `-DICEBREAKER` → `icebreaker_sections.lds`. |
| `picosoc/start.s` | — | Assembly startup: zero regs, zero SPRAM, copy `.data` from flash, zero `.bss`, call `main()`. Also `flashio_worker`. |
| `picosoc/firmware.c` | — | Example firmware (HW–SW interaction); holds the `run_workload()`/`cmd_benchmark_cpi()` harness and flash-mode helpers. |
| `picosoc/ice40up5k_spram.v` | 91 | SPRAM wrapper. 1-cycle data memory (why there's no D-cache). |
| `picosoc/icebreaker.pcf` | — | Pin constraints: FPGA pins → LEDs, UART, flash, 7-seg. |

Tier 3 Files

| File | Lines | Role |
| --- | ---: | --- |
| `picosoc/icebreaker_tb.v` | — | Testbench for simulation (cycle counts). |
| `picosoc/spiflash.v` | — | Behavioural SPI flash model (W25Q-like): QSPI/CRM/DDR timing; used by `icebreaker_tb.v`. Needed to measure cache/fetch-latency changes in sim. |
| `picosoc/Makefile` | — | Build flow: yosys → nextpnr → icepack → iceprog (area + fmax). Firmware compiled `-march=rv32im -mabi=ilp32`. |
| `tests/*.S` | — | 45 RISC-V instruction unit tests (must still pass after edits). |
| `picosoc/simpleuart.v` | 137 | UART. Console output only; not a PPA factor. |

**Key configuration knobs** (parameters in `picorv32.v`): `ENABLE_REGS_16_31`, `ENABLE_REGS_DUALPORT`, `ENABLE_MUL`, `ENABLE_FAST_MUL`, `ENABLE_DIV`, `ENABLE_IRQ`, `ENABLE_COMPRESSED_ISA`, `BARREL_SHIFTER`, `TWO_STAGE_SHIFT`, `TWO_CYCLE_ALU`, `TWO_CYCLE_COMPARE`.

## PicoRV32 Brief Background

`picorv32` is a small multicycle RV32IMC core (CPI 3–5) with a one-hot 8-state FSM. On the iCEBreaker it runs inside PicoSoC: the CPU plus `spimemio` (SPI-flash controller), `simpleuart`, and a 128 KB SPRAM. The defining fact for this project is the memory split: all code executes in-place from SPI flash (reset vector `0x00100000`), while data (`.data`/`.bss`/heap/stack) lives in single-cycle SPRAM. The register file maps to 4 EBR blocks.

Performance is governed by

$$
T_{\text{exec}} = N \times \text{CPI} \times T_c
$$

The firmware is frozen, so the dynamic instruction count $N$ is **fixed** — every hardware change attacks either $T_c$ or CPI. It helps to split CPI into

- CPI **fetch-stall** part (cycles lost waiting for instructions to arrive from flash)
- CPI **core** part (FSM cycles once the word is in hand)

A single flash fetch costs tens of cycles, so the baseline performance is overwhelmingly **fetch-bound**.

### Baseline Resource Usage

Synthesis of the `rv32im` starting point (`COMPRESSED_ISA=0`, `ENABLE_DIV=1`, `ENABLE_FAST_MUL=1`, `BARREL_SHIFTER=0`):

| Resource | Total | Used | Free | Notes |
| --- | ---: | ---: | ---: | --- |
| Logic Cells (LC) | 5280 | ~4251 (80%) | ~1029 | datapath + control + peripherals |
| EBR blocks | 30 | 4 | 26 | register file; main memory is SPRAM |
| DSP blocks | 8 | 4 | 4 | `pcpi_fast_mul` |
| SPRAM blocks | 4 | 4 | 0 | 128 KB data memory |
| Max $f_{\text{clk}}$ | — | ~17.3 MHz | — | baseline critical path was the RVC buffer |

**Key takeaway: logic cells are the binding constraint; memory (EBR/DSP) is abundant.** The ~17.3 MHz figure was measured on the stock `COMPRESSED_ISA=1` baseline (critical path through `mem_16bit_buffer`); the `rv32im` config removes that path, so the real $f_{max}$ must be re-measured but should be higher.

**Resource allocation is essentially forced by the chip.** SPRAM is the only memory large enough for a 128 KB working set (EBR totals just 15 KB); the register file needs the fast dual-read EBR (single-port SPRAM can't supply two operands per cycle); and code lives in the large-but-slow SPI flash. Reshuffling hurts — e.g. moving the register file out of EBR into flip-flops frees 4 (abundant) EBR blocks at the cost of ~1000 (scarce) LCs. So optimisations spend the free EBR/DSP and guard the LC budget.

## Project Requirements and Constraints

The processor is scored on **unknown benchmarks that cannot be modified**, each a `while(1)` loop calling a workload function, with execution time measured via an LED toggle on a Picoscope. Only hardware changes (`.v`/`.sv`) are allowed; all firmware is compiled `-march=rv32im`.

- **Coprocessors are useless** — accelerators (systolic arrays, custom PCPI/MMIO ops) need firmware support the benchmarks won't contain.
- **`COMPRESSED_ISA=0`** — `rv32im` has no C extension, so all instructions are 32-bit. Dropping RVC removes the decoder and half-word alignment logic — saves LUTs and greatly simplifies the fetch stage for pipelining.
- **`ENABLE_DIV=1`** — `rv32im` makes gcc emit `div`/`rem`; without `pcpi_div` those instructions trap.

Only **transparent microarchitectural optimisations** (faster on *any* code, no firmware help) count.


## Benchmarks

We create our own to act as a proxy for an unknown range of benchmarks, covering 2 layers:

- **Microbenchmarks** — each isolates one *core operation* that nearly all code is built from
- **Application programs** — realistic kernels that combine those operations in real proportions

### Harness conventions

Every benchmark — micro or program — follows the project template (`unsigned char run_workload(void)`, frozen `-march=rv32im`) and the same rules, so results are comparable and survive the optimiser:

- **Return a byte checksum folded from all the work**, so dead-code elimination can't delete the kernel.
- **Fixed seeds / static SPRAM inputs** (`xorshift32` for any pseudo-random data) → deterministic `N`, cycle count, and a known-good checksum that doubles as a correctness regression test after each hardware edit.
- **An outer `REPEAT` count** to scale the run into a clean Picoscope window *without changing the instruction mix*.
- **Measurement**: reuse `cmd_benchmark_cpi()` to log `cycles` / `instrs` / `CPI` over UART (sim and board); read wall-clock from the LED1 period on the scope; cross-check against $T_{\text{exec}} = N \times \text{CPI} \times T_c$. `N` is fixed by the frozen source, so every delta is pure $\text{CPI} \times T_c$.
- **Size every benchmark to a similar runtime** so the dashboard reads cleanly, and **record each kernel's code footprint** (`size` of `run_workload`) — it predicts cache behaviour.

### Layer 1 — Core-operation microbenchmarks

Organised by the three sources of CPI (the fetch-stall vs core split above) — fetch, compute, memory, i.e. the three things every instruction does: get fetched, get computed, maybe touch memory. Each is a tight dependent chain reduced to a checksum.

| Bucket | Benchmark | Kernel |
| --- | --- | --- |
| **Compute** | `u_alu` | dependent `add`/`sub`/`and`/`or`/`xor`/`slt` chain |
| | `u_shift` | variable-distance `sll`/`srl`/`sra` |
| | `u_mul` | `mul`/`mulh` chain |
| | `u_div` | `div`/`rem` stream |
| | `u_branch` | data-dependent taken/not-taken branches |
| | `u_call` | leaf-function call/return loop |
| **Memory** | `u_memcpy` | streaming load+store over an SPRAM array |
| | `u_chase` | pointer chase — dependent loads (load latency) |
| **Fetch** | `u_hot` | tiny loop × many iterations (instruction reuse) |
| | `u_cold` | large straight-line / unrolled body, footprint > cache |

The fetch pair matters most for this CPU: `u_hot` and `u_cold` run the same arithmetic at opposite footprints, so together they separate "hot code stayed hot" (cache) from "cold/large code got cheaper" (fast-read).

### Layer 2 — Application programs

One representative kernel per archetype the other teams will plausibly submit. The **core 6** span the main mix axes; the rest extend breadth.

| Archetype | Benchmark | Kernel | Character |
| --- | --- | --- | --- |
| Sorting | `bubble_sort` | sort N ints (the handout example) | branchy, data-dependent, ld/st |
| Linear algebra | `matmul` | 16×16 integer matrix multiply | mul-accumulate, nested loops, streaming |
| Hashing / integrity | `crc32` | CRC-32 over a byte buffer | shift + xor + logic, tight loop |
| Number theory | `prime_count` | count primes ≤ N by trial division | div/mod + branch |
| DSP / filtering | `fir_filter` | fixed-point N-tap FIR over a signal | mul-acc + shift + streaming loads |
| Text | `strsearch` | naive substring search over text | byte loads + branches |
| Simulation | `game_of_life` | Conway, K generations on a grid | 2-D stencil, branch, memory |
| Recursion | `fib_rec` | recursive Fibonacci(n) | call/return + stack ld/st |
| RNG / Monte-Carlo | `xorshift_mc` | PRNG → fixed-point π estimate | shift/xor + compare + mul |

### Results

Each program's mix of microoperations as counts (or % of `N`):

| Program | ALU | Shift | Mul | Div | Branch | Jump | Load | Store | N |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `bubble_sort` | . | . | 0.8 | . | . | . | . | 0.2 | 45 |
| `matmul` | . | . | . | . | . | . | . | . | . |
| ... | | | | | | | | | |

Showing performance of all micro-ops and programs:

```text
design: <branch>                 cycles    CPI    mix (top op classes)
Compute
  u_alu                             .        .
  u_shift                           .        .
  ...
Memory
  u_memcpy                          .        .
Fetch
  u_hot                             .        .
  u_cold                            .        .
Programs
  bubble_sort                       .        .     branch / ld-st / alu
  matmul                            .        .     mul / ld / alu
  ...
```

The **programs' geometric mean is the headline**:

$$
\text{score} = \left(\prod_{i=1}^{n} \frac{T_{\text{base},i}}{T_{\text{opt},i}}\right)^{1/n} \quad\text{(over the programs only)}
$$

geomean rather than arithmetic so one slow kernel can't swamp the figure — standard SPEC practice.

## Design Choices

### Optimization Order

Because the baseline is fetch-bound, Amdahl's Law dictates the dominant term first: **fetch latency before core CPI**. As long as each instruction stalls tens of cycles on flash, cutting core CPI from 4 to 1 gives basically no speedup (hence pipelining is useless for now). So the order is approximately:

- fast-read defaults
- instruction cache
- core-CPI tweaks
- pipelining

PLL optimizations can be applied for free.

| Optimisation | Lever | Effort | Reward | Risk | Done |
| --- | --- | --- | --- | --- | :---: |
| PLL (12 → 24 MHz+) | $T_c$ | low | high | low | ❌ |
| `COMPRESSED_ISA=0` | $T_c$ | low | high | low | ✅ |
| `TWO_CYCLE_ALU/COMPARE` + retiming | $T_c$ | low | cond. | low | ❌ |
| Flash fast-read (Dual/Quad/DDR + CRM) | fetch | low | high | low–med | ❌ |
| Instruction cache (EBR) | fetch | med | high | med | ❌ |
| Loop buffer | fetch | low–med | med | low | ❌ |
| `BARREL_SHIFTER=1` | core CPI | low | med | low | ❌ |
| Fast-path load/store; SRT divider | core CPI | med | med | low–med | ❌ |
| Full 5-stage pipeline | core CPI | high | high | high | ❌ |

### Flash fast-read

The cheapest fetch win: `spimemio` already supports Dual/Quad/DDR + CRM but powers up in the slowest single-bit mode and never leaves it, so a word costs ~64 SPI clocks instead of ~20 (Quad+CRM) or ~8 (sustained sequential address fetches).

The example firmware's `boot()` sets the *flash chip's* QE bit but never switches the *controller* into a fast mode — and cross-evaluation firmware may not touch it at all. So the fix must be in `spimemio`'s **hardware reset defaults** (`config_qspi`/`config_ddr`/`config_cont`/`config_dummy`), guaranteeing fast fetch for any firmware. Three routes, increasing speed and cost:

- **Dual-I/O (`0xBB`)** — uses two data lines, needs **no flash configuration bit**, near-zero logic, roughly halves the transfer.
- **Quad-I/O (`0xEB`)** — needs a small reset-time boot FSM to set the flash QE bit (`WREN`+`WRSR`) before the first fetch; unlocks the full ~3.5× speedup.
- **Quad DDR (`0xED`)** — fastest (clocks data on both edges); the DDR datapath already exists in `spimemio`, so the only cost is DDR I/O timing closure.

**Deeper prefetch (line buffer).** `spimemio`'s streamer keeps sequential reads cheap but restarts the full command+address phase on *every* non-sequential fetch, so each taken branch/call/return pays the worst-case latency. A small fully-associative line buffer (a few recently-fetched lines) inside `spimemio` absorbs short backward branches — a partial, few-LC substitute for the CPU-side cache below.

### Instruction cache

Loops re-fetch the same instructions every iteration; an EBR-backed cache serves repeats in **1 cycle**, and the `while(1)` structure means a ~100% hit rate after the first iteration. (Complementary to fast-read, which only shrinks the cold-miss penalty.) Running code directly from SPRAM is *not* an option — the linker script pins `.text` to flash, SPRAM is fully allocated to data, and SPRAM isn't pre-loadable — so "execute from SPRAM" collapses to exactly this bus-resident cache.

It sits between the CPU memory interface and `spimemio`: on a hit it returns the word and asserts `mem_ready` immediately (intercepting the memory bus FSM at its idle state); on a miss it forwards to flash, caches the returned word, then responds. `mem_instr` keeps it instruction-only (no D-cache needed — SPRAM is already 1-cycle).

**Proposed:** a direct-mapped, 256-entry, 1-word-per-block cache (1 KB) using ~2 EBR for data, with the address split as `tag[31:10]` / `set[9:2]` / `byte[1:0]`. The tag array holds 256 × (1 valid + 22 tag) bits (a small LUT-RAM or one EBR). Scaling knobs if needed: larger capacity (e.g. 4 KB / ~8 EBR) cuts capacity misses; 2-way cuts conflict misses where two hot addresses alias; multi-word blocks add spatial locality at a higher miss penalty. Instruction caches are read-only, so no write policy is needed.

A **loop buffer** (loop-stream detector, no tag array) is a cheaper fallback if LCs get tight — same ~100% hit rate for tight loops, but no help for straight-line or nested code.

### Incremental CPI reductions

Smaller wins within the existing multicycle FSM — stepping stones or pipeline fallbacks:

- **`BARREL_SHIFTER=1`** — shifts finish combinationally (3 cyc) instead of iterating (up to ~14). ~200 LCs. Also a **prerequisite for pipelining** (a fixed-latency Execute stage can't tolerate the shift loop).
- **Fast-path load/store** — loads/stores cost 5 cycles because the memory states are entered twice for the generic handshake. Data always targets 1-cycle SPRAM, so a fast path can collapse the second visit, saving a cycle per access with no pipeline machinery.
- **Radix-4 SRT divider** — `pcpi_div` retires 1 quotient bit/cycle (~32 cyc); radix-4 does 2 bits/cycle (~16) for modest LCs. Helps any divide-bearing benchmark. (Multiply is already DSP-backed — leave it.)

### Pipelining

The full core-CPI win (CPI→~1), but the riskiest. Approach: incrementally pipeline the *existing* datapath into 5 stages, reusing the nonarchitectural registers (`reg_op1`, `reg_op2`, `alu_out_q`, `reg_out`) as pipeline registers.

| Stage | picorv32 origin | Moves here |
| --- | --- | --- |
| Fetch | `fetch` + cache | cache lookup; stall on miss |
| Decode | `ld_rs1` | decode, regfile read, immediate |
| Execute | `exec` | ALU/branch/JALR (barrel shifter required); PCPI handshake |
| Memory | `ldmem`/`stmem` | SPRAM access (1-cyc) |
| Writeback | deferred WB | regfile write |

Needs separate instruction/data paths (cache + SPRAM, distinguished by `mem_instr`), pipeline registers, **forwarding** (Mem/WB → Execute), and a **hazard unit** (load-use stall, branch flush). Four picorv32-specific challenges: deferred writeback must become a real WB stage; iterative shifts need the barrel shifter; PCPI mul/div must insert bubbles; the two-cycle decode must fit one Decode stage. Rough cost +1500–2000 LCs (with the cache, ~3500–4500 / 5280 — tight but feasible). **Branch prediction (BTB + RAS) is deferred** — it only pays once the pipeline exposes the control bubble that the cache otherwise hides.

### Clock frequency and critical path

- **Target:** 24 MHz via PLL (36–48 stretch). A faster $f_{clk}$ also speeds the flash interface in wall-clock terms, so it helps even fetch-bound code.
- **Hard ceilings:** SPRAM 70 MHz and DSP 50 MHz cap $f_{max}$ at ~48 MHz regardless of logic path.
- **`TWO_CYCLE_ALU/COMPARE`:** shorten the critical path but add a cycle per ALU op on the multicycle core — best viewed as a pipeline stage-balancing tool, not a free win.

### Power and Area (alternative targets)

Energy per task is $E = P \times t$, so a faster design that finishes and idles can use *less* energy despite higher peak power ("race to idle") — the PLL and fetch fixes often improve efficiency.

**Power moves:**

- Clock-gate the DSP multiplier (`MUL_CLKGATE`).
- Put untouched SPRAM banks in standby/sleep.
- Operand isolation — hold ALU/DSP inputs stable when the result is unused.
- Quieter flash (a side benefit of fast-read/cache, less pin toggling).

**Area moves** (opposed to speed):

- Strip `ENABLE_IRQ`, `ENABLE_COUNTERS`, two-stage shift, the dual-port regfile, and the misalign/illegal-instruction catchers.
- Dropping `pcpi_fast_mul` frees 4 DSP but is unsafe unless the benchmark provably never multiplies.

### Rejected alternatives

- **Coprocessors / systolic array** — idle without firmware that drives their MMIO; SPRAM's single port blocks CPU+array sharing.
- **Multiprocessing** — two cores ~doubles LCs and needs bus arbitration; benefit needs a parallelisable benchmark.
- **Hardware multithreading** — hides latency, but the dominant latency is shared flash; a cache fixes it better.
- **Replacing the core** — higher ceiling, but re-wiring PicoSoC/MMIO, re-validating the tests, and the lost understanding aren't worth it. We modify `picorv32`.
