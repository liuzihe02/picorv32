# GB3 RISC-V Project

This is a design document containing brief background and implementation plan for the GB3 RISC-V Project. For detailed technical background on microarchitecture and details of `picorv32`, please refer to the [technical deep dive](guide/guide.pdf) that Zach maintains.

## Tips

- Zach reccomends using [markdown-preview-enhanced](https://shd101wyy.github.io/markdown-preview-enhanced/#/) to view md files.
  - This also supports `tikz` diagrams. Use latex engines like `latexmk` or `pdf2svg` or whatever to render these in md, and then do `ctrl + shift + enter` to render them all at once
- Instead of using `screen /dev/ttyUSB1 115200` which can screw up scrolling with mouse and copying, install and use `tio -b 115200 /dev/ttyUSB1` which simply prints to terminal

## Core Files (For LLM context)

> Please have your coding agent read the below files (at least tier 1 files) first before asking it anything, if not it will probably do stupid shit.

This will almost always require a model with at least 500K token context window. Files are tiered by relevance to the committed optimisation plan (fetch-bound first → fast-read + I-cache, then core CPI, then pipeline; PLL free; coprocessors out).

Tier 1 Files

| File | Role |
| --- | --- |
| `picorv32.v` | CPU core. Main module `picorv32`; also `picorv32_pcpi_mul/fast_mul/div`, `picorv32_regs`, `picorv32_axi`, `picorv32_wb`. |
| `picosoc/spimemio.v` | SPI flash controller — the dominant fetch-latency block: fast-read reset defaults, jump penalty, line-buffer insertion point. |
| `picosoc/icebreaker.v` | Top-level for the iCEBreaker board: instantiates `picosoc` (iCE40UP5K config, SPRAM, 7-seg; no PLL, raw 12 MHz). Current rv32im params: `BARREL_SHIFTER=0`, `ENABLE_MUL=0`, `ENABLE_DIV=1`, `ENABLE_FAST_MUL=1`, `ENABLE_COMPRESSED=0`, `ENABLE_ICACHE` (cache on/off). |
| `picosoc/picosoc.v` | SoC wrapper. Wires CPU to SRAM, UART, SPI flash, GPIO. Address decode + memory map live here; the `icache` sits on this bus between the CPU and `spimemio`. |
| `picosoc/icache.v` | Instruction cache (added): parametric `SETS`×`WAYS`×`WORDS_PER_LINE` (direct-mapped or set-associative, multi-word lines), EBR-backed. Currently built 2-way × 4-word × 256-set. Intercepts the flash-region fetch path. |
| `README.md` | Full documentation of all configuration parameters. |
| `picosoc/README.md` | picosoc documentation. |
| `picosoc/benchmarks.c` | benchmark suite |
| `picosoc/benchmarks.h` | benchmark suite header file. |

Ideally also get it to read the [technical deep dive](guide/guide.tex)

Tier 2 Files

| File | Role |
| --- | --- |
| `picosoc/sections.lds` | Linker script: `.text`/`.rodata` → FLASH (`0x00100000`), `.data`/`.bss`/`.heap` → SPRAM — the memory map (code in flash = the fetch-bound reality). Preprocessed `-DICEBREAKER` → `icebreaker_sections.lds`. |
| `picosoc/start.s` | Assembly startup: zero regs, zero SPRAM, copy `.data` from flash, zero `.bss`, call `main()`. Also `flashio_worker`. |
| `picosoc/firmware.c` | Example firmware (HW–SW interaction); holds the `run_workload()`/`run_scope()` harness and flash-mode helpers. |
| `picosoc/ice40up5k_spram.v` | SPRAM wrapper. 1-cycle data memory (why there's no D-cache). |
| `picosoc/icebreaker.pcf` | Pin constraints: FPGA pins → LEDs, UART, flash, 7-seg. |

Tier 3 Files

| File | Role |
| --- | --- |
| `picosoc/icebreaker_tb.v` | Testbench for simulation (cycle counts). |
| `picosoc/icache_tb.v` | Standalone `icache` unit test vs a streaming golden-flash model; sweeps several geometries/latencies in one run (`make icache_tb`). Asserts data correctness + hit/miss behaviour + invalidation. |
| `picosoc/spiflash.v` | Behavioural SPI flash model (W25Q-like): QSPI/CRM/DDR timing; used by `icebreaker_tb.v`. Needed to measure cache/fetch-latency changes in sim. |
| `picosoc/Makefile` | Build flow: yosys → nextpnr → icepack → iceprog (area + fmax). Firmware compiled `-march=rv32im -mabi=ilp32`. |
| `tests/*.S` | 45 RISC-V instruction unit tests (must still pass after edits). |
| `picosoc/simpleuart.v` | UART. Console output only; not a PPA factor. |

**Key configuration knobs** (parameters in `picorv32.v`): `ENABLE_REGS_16_31`, `ENABLE_REGS_DUALPORT`, `ENABLE_MUL`, `ENABLE_FAST_MUL`, `ENABLE_DIV`, `ENABLE_IRQ`, `COMPRESSED_ISA`, `BARREL_SHIFTER`, `TWO_STAGE_SHIFT`, `TWO_CYCLE_ALU`, `TWO_CYCLE_COMPARE`. (PicoSoC wraps these under `ENABLE_COMPRESSED` etc., and adds its own `ENABLE_ICACHE` flag — see `picosoc.v`.)

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

Every benchmark — micro or program — is a `uint8_t bench_NAME(void)` (frozen `-march=rv32im`) following the same rules, so results are comparable and survive the optimiser. The scored workload is selected by `run_workload()` in `firmware.c`, which calls one `bench_*`:

- **Return a byte checksum folded from all the work**, so dead-code elimination can't delete the kernel.
- **Fixed seeds / static SPRAM inputs** (`xorshift32` for any pseudo-random data) → deterministic `N`, cycle count, and a known-good checksum that doubles as a correctness regression test after each hardware edit.
- **An inner loop count** (a literal in each kernel, e.g. `50000`) scales the run into a clean Picoscope window *without changing the instruction mix*.
- **Measurement**: `time_benchmark()` logs `cycles`/`instrs`/`CPI` over UART — per-suite via `run_benchmarks()` (menu `T`) or the scored path `run_scope()` (menu `B`, loops `run_workload()` + toggles LED1). **Wall-clock**, two ways:
  - **A — UART, any benchmark:** $t = \text{cycles}/f_{clk}$, but no hand math — the dashboard prints an `ms` column (`print_ms()`, driven by `F_CLK_HZ` in `benchmarks.h`, the single source of truth that also sets the UART divisor). Cleanest for the kernel; `time_benchmark` brackets only `fn()`.
  - **B — scope, ground-truth:** LED1 is a square wave → wall-clock/pass = period/2; auto-accounts for `f_clk`, but only the one scored workload.

  They should agree; if not, `f_clk` is off (back it out as $f_{clk} = \text{cycles}/(\text{period}/2)$). `N` is frozen, so every delta is pure $\text{CPI} \times T_c$.
- **Size every benchmark to a similar runtime** so the dashboard reads cleanly. Each kernel's code footprint (`size` of its `bench_*`) predicts cache behaviour.

### Layer 1 — Core-operation microbenchmarks

Organised by the three sources of CPI (the fetch-stall vs core split above) — fetch, compute, memory, i.e. the three things every instruction does: get fetched, get computed, maybe touch memory. Each is a tight dependent chain reduced to a checksum.

| Bucket | Benchmark | Kernel |
| --- | --- | --- |
| **Compute** | `bench_alu` | dependent `add`/`sub`/`and`/`or`/`xor`/`slt` chain |
| | `bench_shift` | variable-distance `sll`/`srl`/`sra` |
| | `bench_mul` | `mul`/`mulhu` chain |
| | `bench_div` | `div`/`rem` stream |
| | `bench_branch` | data-dependent taken/not-taken branches |
| | `bench_call` | leaf-function call/return loop |
| **Memory** | `bench_memcpy` | streaming load+store over an SPRAM array |
| | `bench_chase` | pointer chase — dependent loads (load latency) |
| **Fetch** | `bench_hot` | tiny loop × many iterations (instruction reuse) |
| | `bench_cold` | large straight-line / unrolled body, footprint > cache |

The fetch pair matters most for this CPU: `bench_hot` and `bench_cold` run the same arithmetic at opposite footprints, so together they separate "hot code stayed hot" (cache) from "cold/large code got cheaper" (fast-read).

**Planned — non-sequential fetch (the QSPI gap).** With the cache on, `bench_hot` and all 6 programs hit ~100% (~6.4 CPI), so flash fast-read surfaces *only* in `bench_cold` — which isn't in the scored geomean. There are two cache-miss regimes, and only the first is covered:

- **Sequential** (straight-line > cache, e.g. `bench_cold`): consecutive misses stream from `spimemio` (data phase only), so fast-read shrinks just that phase.
- **Non-sequential / jumpy** (working set > cache): every miss re-runs the full command + address + dummy + data — ~4–8× costlier per miss, the regime where fast-read helps *most* and the only one where CRM pays off. Covered by `bench_interp` (Layer 2).

A graded-footprint ladder (`cold` unrolled to < / ≈ / ≫ cache) was considered but maps the *cache-size* knee, not fetch speed: sequential code that overflows the cache misses at a constant per-word rate however far it overflows, so a bigger body is the same CPI, just longer. Park it for when we evaluate growing the cache.

### Layer 2 — Application programs

One representative kernel per archetype the other teams will plausibly submit, all implemented in `benchmarks.c`. The **core 6** span the main mix axes; the last four extend breadth — notably `bench_interp`, the one kernel large/jumpy enough to miss the I-cache (so the scored geomean can see the fetch lever and CRM).

| Archetype | Benchmark | Kernel | Character |
| --- | --- | --- | --- |
| Sorting | `bench_bubble_sort` | sort N ints (the handout example) | branchy, data-dependent, ld/st |
| Linear algebra | `bench_matmul` | 16×16 integer matrix multiply | mul-accumulate, nested loops, streaming |
| Hashing / integrity | `bench_crc32` | CRC-32 over a byte buffer | shift + xor + logic, tight loop |
| Number theory | `bench_prime_count` | count primes ≤ N by trial division | div/mod + branch |
| DSP / filtering | `bench_fir` | fixed-point 16-tap FIR over a signal | mul-acc + shift + streaming loads |
| Text | `bench_strsearch` | naive substring search over text | byte loads + branches |
| Interpreter / VM | `bench_interp` | bytecode dispatch: data-dependent `switch` over 32 ops, handler code > cache | non-sequential fetch; **the one kernel that misses the I-cache**, making the geomean sensitive to fast-read + CRM |
| Simulation | `bench_game_of_life` | Conway, K generations on a grid | 2-D stencil, branch, memory |
| Recursion | `bench_fib_rec` | recursive Fibonacci(n) | call/return + stack ld/st |
| RNG / Monte-Carlo | `bench_xorshift_mc` | PRNG → fixed-point π estimate | shift/xor + compare + mul |

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
  bench_alu                         .        .
  bench_shift                       .        .
  ...
Memory
  bench_memcpy                      .        .
Fetch
  bench_hot                         .        .
  bench_cold                        .        .
Programs
  bench_bubble_sort                 .        .     branch / ld-st / alu
  bench_matmul                      .        .     mul / ld / alu
  ...
```

The **programs' geometric mean is the headline**:

$$
\text{score} = \left(\prod_{i=1}^{n} \frac{T_{\text{base},i}}{T_{\text{opt},i}}\right)^{1/n} \quad\text{(over the programs only)}
$$

geomean rather than arithmetic so one slow kernel can't swamp the figure — standard SPEC practice.

Baseline figures:

```txt
benchmark (cycles  instrs  CPI  checksum)
-- Compute --
alu           125052910   1850038   67.59   82
shift         115452462   1700031   67.91   0
mul           137852910   2050038   67.24   124
div           84682526   1240032   68.29   183
branch        222790461   2974936   74.88   64
call          290952270   4100028   70.96   32
-- Memory --
memcpy        66409751   930068   71.40   64
chase         64565303   904386   71.39   136
-- Fetch --
hot           282602206   4000027   70.65   117
cold          65925462   1029530   64.03   239
-- Programs --
bubble_sort   52748123   756032   69.76   77   branch/ld-st
matmul        201222585   2922496   68.85   204   mul/ld-st
crc32         223126030   3075453   72.55   0   shift/xor
prime_count   45665871   521375   87.58   141   div/branch
fir           301194379   4328124   69.59   214   mul/shift
strsearch     127429678   1568299   81.25   200   ld/branch
done
```

## Competition Strategy: offense & defense

Each group submits a **secret benchmark** for cross-evaluation: our CPU is scored on *others'* unknown benchmarks, and ours runs on *theirs*. Many groups will build a cache-busting benchmark hoping a cache-based CPU stalls catastrophically. This is the war-game — what they can actually attack, why their best attack mostly favours *us*, and how we close the one flank that doesn't.

### The battlefield is instruction fetch — data is immune

The entire attack surface is the **instruction-fetch stream**. All data (`.data`/`.bss`/heap/stack) lives in 128 KB SPRAM that answers in **1 cycle regardless of footprint** — there is no D-cache to thrash. The classic killer (pointer-chase over a huge array) does nothing to us; it would wreck a group that put a small D-cache over slow memory. So every "make it fail" benchmark has exactly one lever: the **code (and `.rodata`, also flash-resident) footprint and access pattern** of `run_workload`.

The build constraint bounds their *weapons* too: `-nostdlib -march=rv32im` means floats and 64-bit division emit libgcc calls that **won't link** — adversaries are confined to 32-bit-integer hardware ops + fetch patterns. On a clean board, "completely fail" means *catastrophically slow*, not *wrong* (one correctness fragility noted below).

### The counterintuitive core: cache-busters favour us

When a benchmark forces constant I-cache misses, **caching stops mattering for everyone** and it collapses into a pure **fetch-bandwidth contest** — which only we optimised. Most groups cached and *stopped*, leaving flash in the slow single-bit power-up mode (the example firmware never switches it). So their hardest cache-buster hurts them more than us:

- our miss = qddr fetch (~4× cheaper than single-SPI); sequential misses stream cheaper still
- their miss = ~64–128-cycle single-SPI fetch

**qddr is an asymmetric moat** — the harder they make the benchmark, the more it rewards the fetch optimisation nobody else did.

### Our one real flank, and the threat matrix

Our genuine exposure is narrow: a footprint that fits a *bigger* competitor cache but not our **1 KB direct-mapped** one (window `(1 KB, their-cache]`), or **conflict-aliasing** into our 256 direct-mapped sets. Both stem from our cache being needlessly small — we use **7/30 EBR (23 idle)**.

| Rival profile | Benchmark that beats us | Exposure |
| --- | --- | --- |
| Cache-only, **no fast-read** (most groups) | none fetch-bound | low — qddr moat |
| Bigger cache **+ fast-read** | footprint in `(1 KB, their-cache]` | **medium — the real flank** |
| **Pipelined** core | compute-bound loop that fits cache (~3 CPI us vs ~1) | high but rare |
| **Radix-4** divider | `div`/`rem`-bomb (we're radix-2, ~40 cyc) | medium — cheap to fix |

(Conflict-aliasing aside: two hot addresses 1 KB apart map to the same direct-mapped line and evict each other every iteration → 100% miss on a loop that "should" fit. The cheapest small-effort attack on a direct-mapped cache — and the reason associativity matters defensively even though it's worthless for our own 100%-hit kernels.)

### Defense (priority-ordered)

1. **Spend the 23 idle EBR — grow the cache.** The #1 move; our cache is small by accident, not constraint — capacity collapses the footprint window, associativity kills the aliasing attack. Design + staged rollout: [Further Optimizations → Implementation plan](#further-optimizations).
2. **Radix-4 divider** — cheap insurance against the div-bomb (the only non-fetch flank).
3. **qddr fast-read** ✅ — the moat; keep it the committed default.
4. **Fragility note:** the cache *immortalises a transient flash-read error* (see firmware engineering log). Not adversary-triggerable through `run_workload` on a clean board, but it's the one place a cache can fail where a no-cache design self-heals.

> Note the reframing vs the [Instruction cache → Further Optimizations](#further-optimizations) section: capacity/associativity is ~worthless for *our* 100%-hit kernels, but it is the *primary defense* against adversarial benchmarks. Different question, different answer.

### Offense: the benchmark we submit

> A **large, fetch-bound, sequential-streaming** kernel whose code footprint **exceeds the entire 15 KB EBR** — uncacheable by *anyone* — folded to a checksum.

- **Footprint > 15 KB:** removes caching for everyone (incl. bigger-cache rivals), so our small cache is irrelevant and it's the pure fetch contest we win. (Sizing it just above 1 KB is a self-own — a 4 KB-cache rival fits it; go past everyone.)
- **Sequential, not jumpy:** sequential misses stream at qddr data-phase (~4× their single-SPI), and our 1-word cache still streams cheaply through `spimemio`. Jumpy code (interpreter) pays full command+address *per jump* for us too, shrinking the edge.
- **Fetch-bound, not compute/div-bound:** a compute-bound loop feeds pipelined rivals (~1 CPI); a div-bomb feeds radix-4 rivals. Keep per-instruction work light so *fetch* dominates — where we're unique.
- **Differentiation:** a cache-only rival's speedup-vs-baseline ≈ **1×** (cache can't hold it, no fast-read); ours ≈ **4×** (qddr). Wins under both absolute-wall-clock and self-relative-speedup scoring.

Dress it as a real kernel (large unrolled DSP/crypto pipeline, or a generated state machine) so it isn't dismissed as gaming — `bench_cold` is already this shape; scale its body past 16 KB and keep it sequential. (`bench_interp` is the jumpy cousin: a good *defensive* miss-path test, weaker *offensive* pick.)

## Design Choices

### Optimization Order

`T_exec = N × CPI × T_c`, with `N` frozen (we cant modify compiler) and `CPI = CPI_fetch + CPI_core`. `CPI_core` is the CPU system CPI, while `CPI_fetch` is CPI due to making SPI fetches. Then there are **three** levers: fetch-stall CPI, core CPI, and clock period `T_c`. Amdahl's Law sets the order — the baseline is ~70 CPI and almost all of it is fetch-stall:

> As long as each instruction stalls tens of system cycles on flash, cutting core CPI from 4 to 1 gives basically no speedup (hence pipelining is useless *for now*). Fixed by the instruction cache =)

**1. Fetch latency first**

- instruction cache ✅ — serves hot-loop repeats in ~2 cyc instead of ~128
- SPI flash fast-read defaults ✅ — dual/quad/qddr reset defaults all shipped (qddr committed); shrinks the cold-miss penalty, complementary to the cache (CRM still parked)

Until fetch is cheap, the other two levers are *invisible*: core-CPI and clock wins are overwhelmed by horrible flash stall.

**2. Then core CPI and clock** — only worth attacking once fetch is cheap; two *independent* levers:

- **Core CPI:**
  - `BARREL_SHIFTER` ✅ 
  - fast-path load/store
  - radix-4 divider
  - full 5-stage pipeline (`CPI_core` → ~1)
- **Clock `T_c`:**
  - PLL ✅ (currently ~18.4 MHz), already maxed against the ~18.5 MHz logic critical path.
  - Going higher needs the path *shortened* first (`TWO_CYCLE_ALU`/retiming)

The PLL is gated on the critical path. But it's the highest-leverage lever once unlocked (~2.8× at 50 MHz, speeds even fetch-bound code), so critical-path work (`TWO_CYCLE_ALU`, pipelining) pulls double duty: lower core CPI *and* a higher clock.

| Optimisation | Lever | Effort | Reward | Risk | Done |
| --- | --- | --- | --- | --- | :---: |
| PLL (12 → ~18.4 MHz) | $T_c$ | low | high | low | ✅ |
| `COMPRESSED_ISA=0` | $T_c$ | low | high | low | ✅ |
| `TWO_CYCLE_ALU/COMPARE` + retiming | $T_c$ | low | cond. combine w/higher clock.| low | ❌ |
| Look-ahead decode (`LOOKAHEAD_DECODE`, Step 1a-i) | $T_c$ | med | low (~+7% fmax, measured) | low | ✅ |
| Flash fast-read (Dual/Quad/DDR; CRM parked) | fetch | low | high | low–med | ✅ |
| Instruction cache (EBR) | fetch | med | high | med | ✅ |
| Loop buffer | fetch | low–med | med | low | ❌ |
| `BARREL_SHIFTER=1` | core CPI | low | low | low | ✅ |
| Fast-path load/store; SRT divider | core CPI | med | med | low–med | ❌ |
| Full 5-stage pipeline | core CPI | high | high | high | ❌ |

### Flash fast-read

> **Status: shipped.** Dual / Quad / Quad-DDR reset defaults all implemented and verified on board; mode 3 (qddr) is the committed `FLASH_INIT_MODE` in `icebreaker.v`. The flash QE bit is set in hardware by `spimemio`'s reset init FSM (states 13–17), so fast fetch holds for any firmware. CRM remains parked.

The cheapest fetch win: `spimemio` already supports Dual/Quad/DDR + CRM but powers up in the slowest single-bit mode and never leaves it, so a word costs ~64 SPI clocks instead of ~20 (Quad+CRM) or ~8 (sustained sequential address fetches).

The example firmware's `boot()` sets the *flash chip's* QE bit but never switches the `spimemio` *controller* into a fast mode — and cross-evaluation firmware may not touch it at all. So the fix must be in `spimemio`'s **hardware reset defaults** (`config_qspi`/`config_ddr`/`config_cont`/`config_dummy`), guaranteeing fast fetch for **any** firmware. The 4 config modes are

- **Single-bit (`0x03`)** — one lane → 1×, the slow power-up default
- **Dual-I/O (`0xBB`)** — two lanes, no flash QE bit needed → ~2×, near-zero logic
- **Quad-I/O (`0xEB`)** — four lanes → ~3.5×, but the flash QE bit must be set first (Rollout step 2).
- **Quad DDR (`0xED`)** — clocks data on both edges → fastest, but adds DDR I/O timing closure (Rollout step 3).

#### Mode encoding (how the bits map)

Two things set the speed: **lanes** (how many IO wires carry data) and **rate** (SDR = data on the rising SCK edge only; DDR = both edges). `INIT_MODE` picks a point in that grid, which `spimemio` decodes into the `{config_ddr, config_qspi}` cfgreg bits and the read command:

| `INIT_MODE` | `{ddr,qspi}` | cmd | lanes × rate | dummy | bits/SCK | SPI-state print |
| ---: | :---: | :---: | :--- | ---: | ---: | :--- |
| 0 single | `00` | `0x03` | 1 × SDR | 8 (unused) | 1 | DDR off, QSPI off |
| 1 dual | `10` | `0xBB` | 2 × SDR | 0 | 2 | **DDR on**, QSPI off |
| 2 quad | `01` | `0xEB` | 4 × SDR | 4 | 4 | DDR off, QSPI on |
| 3 qddr | `11` | `0xED` | 4 × DDR | 7 | 8 | DDR on, QSPI on |

- **Naming trap — `config_ddr` is *not* "double-data-rate".**
  - It's cfgreg bit 22, really *mode-select bit 1*
  - That's why `cmd_print_spi_state` reads "DDR on" for dual — true double-rate is *only* `{11}` qddr (`0xED`)
  - dual is 2 lanes at single rate, nothing to do with clock edges.
  - In `spimemio.v`: `xfer_dspi = ddr & !qspi` is the dual-SDR path; `xfer_ddr = ddr & qspi` is the only true-DDR path.
- **TOO COMPLICATED - CRM is a separate switch** (cfgreg bit 20 / `config_cont` / `INIT_CRM`)
  - it drops the 8-clock command byte on each *non-sequential* refetch by sending mode byte `0xA5` instead of `0xFF`.
  - **Only works with dual/quad/qddr** (single sends no mode byte, so CRM doesn't apply).
  - *ran into many problems implementing this in hardware so we leave this for now*
- **Measuring per-mode on the board:**
  - `boot()` sets the flash QE bit first via `set_flash_qspi_flag()`, so you can switch live via the firmware menu (`[3]`–`[7]`) and run `[T]`/`[B]` under each mode
  - The QE-init FSM (rollout step 2 below) is needed *only* to make quad safe as the firmware-independent **reset default**, not to measure it.

#### Implementation plan: one top-level knob

Rather than hand-edit `spimemio.v` per experiment, parameterise its reset defaults *once* and drive everything from a single mode knob at the top level (same pattern as `ENABLE_ICACHE`):

- **`spimemio.v`**:
  - add params `INIT_MODE` (2-bit: `0`=single / `1`=dual / `2`=quad / `3`=qddr) and `INIT_CRM`.
  - The `!resetn` block derives `{config_ddr, config_qspi, config_dummy}` from `INIT_MODE` via a `case`, and `config_cont <= INIT_CRM`.
  - Defaults `INIT_MODE=0, INIT_CRM=0` reproduce today's single-SPI exactly — a no-op until flipped.
- **`picosoc.v`**:
  - add pass-through params `FLASH_INIT_MODE` / `FLASH_INIT_CRM`, wired to the `spimemio` instance.
- **`icebreaker.v`**:
  - set `FLASH_INIT_MODE` / `FLASH_INIT_CRM` on the `picosoc` instance — the only line touched per experiment.

A single `INIT_MODE` enum (not four raw bits) is deliberate: `config_dummy` **must** match the command (`0xBB`→0, `0xEB`→4, `0xED`→7) or the data phase mis-aligns and fetches garbage. The enum derives the correct dummy internally, so an illegal combo is unrepresentable. (Dummy values are lifted from the known-good firmware `set_flash_mode_*` setters.)

#### Rollout (staged by risk) — **all shipped**

1. **Dual (`FLASH_INIT_MODE=1`) ✅ — first.**
   1. Firmware-independent, **no QE bit needed** (uses IO0/IO1, the always-on data pins), ~2× transfer, near-zero logic. The safe baseline win.
2. **Quad (`FLASH_INIT_MODE=2`) ✅ — QE-init FSM added.**
   1. `0xEB`/`0xED` use IO2/IO3, which on the W25Q128JV default to `/WP` and `/HOLD`; they only become data lines when the flash's **QE bit (SR2 bit 1, S9)** is set — and that bit lives *inside the flash*, not the FPGA, so the read command alone can't enable it.
> **Why an FSM and not firmware:** software *can* set QE (that's `set_flash_qspi_flag()` in `boot()`, which is how the live menu measures quad) — but `boot()` runs *after* the reset-vector fetch, and a quad reset default fetches that first instruction in quad mode before any firmware executes. Too late. So the controller sets QE itself: `spimemio`'s reset init chain (`0xFF`→`0xAB`) **now also issues** `0x50` (write-enable volatile) + `0x31` (WRSR2, QE=1) in states 13–17, each CS-framed, before the first read. Volatile = instant, no flash-endurance wear, re-applied every reset → works for any firmware.
3. **Quad DDR (`FLASH_INIT_MODE=3`) ✅ — DDR timing closed on board (committed default).** Same QE requirement (covered by the same FSM) *plus* DDR I/O timing on the negedge `xfer_io*_90` path — which held up on the board.

**Verified:** `make icebsim` boots clean in every mode (`spiflash.v` model handles `0xBB`/`0xEB`/`0xED` with matched dummy), the 45 `tests/*.S` are unaffected (flash-path change only), and quad/qddr were confirmed on board via `run_flashmodes` / `run_scope`.

performance with `qddr` and no `crm` (makes a super tiny difference anyway I tested it)
```txt
clock = 17.250 MHz

benchmark     |     cycles |   instrs |    CPI | wallclock (ms) | chk | mix
--------------+------------+----------+--------+----------------+-----+----
-- Compute --
alu           |   12750633 |  1850038 |   6.89 |        739.167 |  82 | 
shift         |   11813079 |  1700031 |   6.94 |        684.816 |   0 | 
mul           |   12700665 |  2050038 |   6.19 |        736.270 | 124 | 
div           |   11360572 |  1240032 |   9.16 |        658.583 | 183 | 
branch        |   21062956 |  2974936 |   7.08 |       1221.040 |  64 | 
call          |   27600667 |  4100028 |   6.73 |       1600.038 |  32 | 
-- Memory --
memcpy        |    6200037 |   930068 |   6.66 |        359.422 |  64 | 
chase         |    6128664 |   904386 |   6.77 |        355.284 | 136 | 
-- Fetch --
hot           |   26800532 |  4000027 |   6.70 |       1553.654 | 117 | 
cold          |    8260938 |  1029530 |   8.02 |        478.894 | 239 | 
-- Programs --
bubble_sort   |    4831139 |   756032 |   6.39 |        280.066 |  77 | branch/ld-st
matmul        |   18776005 |  2922496 |   6.42 |       1088.464 | 204 | mul/ld-st
crc32         |   19704453 |  3075453 |   6.40 |       1142.287 |   0 | shift/xor
prime_count   |    5227904 |   521375 |  10.02 |        303.066 | 141 | div/branch
fir           |   27799599 |  4328124 |   6.42 |       1611.570 | 214 | mul/shift
strsearch     |   10514874 |  1568299 |   6.70 |        609.557 | 200 | ld/branch
interp        |   26100794 |  3909943 |   6.67 |       1513.089 |  78 | jump/branch
game_of_life  |  150108128 | 23041099 |   6.51 |       8701.920 |  33 | branch/ld-st
fib_rec       |   18070777 |  2755365 |   6.55 |       1047.581 | 179 | call/ld-st
xorshift_mc   |   58534341 |  8235354 |   7.10 |       3393.295 |  65 | mul/shift
done
```

#### Sim model (`spiflash.v`) gotchas

The sim flash model has to agree with the controller on **two** things per mode, or `icebsim` lies:

1. **Dummy cycles must match `config_dummy`.** After the address+mode byte, the model tristates the bus for its dummy count while the controller starts clocking for *data* on its own `config_dummy` schedule. If they differ, the controller samples `Z`→`x` and latches a misaligned/garbage word. So the model's per-command dummy must equal the controller's: `0xBB`→**0**, `0xEB`→**4**, `0xED`→**7**. The stock model hardcoded a generic `latency`=8 for *all* fast-read commands — wrong for every one of ours. **Now fixed for all three:** `0xBB`→`0` (the M7–M0 mode byte *is* the turnaround on the W25Q128JV — no separate dummy clocks), `0xEB`→`4`, `0xED`→`7`, matching `config_dummy`, so `icebsim` aligns the data phase in every mode.
2. **The model does *not* model the QE bit.** It ignores the `0x35`/`0x31`/`0x50`/`0x06` register sequence and answers `0xEB`/`0xED` regardless of QE state. So quad "works" in sim without ever setting QE — meaning **sim cannot validate the QE-init FSM** (rollout step 2); only the board can. A green `icebsim` for quad is *not* proof the QE sequence is right.

**Deeper prefetch (line buffer).** `spimemio`'s streamer keeps sequential reads cheap but restarts the full command+address phase on *every* non-sequential fetch, so each taken branch/call/return pays the worst-case latency. A small fully-associative line buffer (a few recently-fetched lines) inside `spimemio` absorbs short backward branches — a partial, few-LC substitute for the CPU-side cache below.

### Instruction cache

Loops re-fetch the same words every iteration, so an EBR-backed cache that answers a repeat in **2 cycles** — instead of the many tens a flash word costs — collapses the fetch-bound baseline (~70 CPI) to **~6.4 CPI**.

Running from SPRAM instead is impossible (`.text` is pinned to flash, SPRAM is full of data and isn't pre-loadable), so the only fast instruction store is this bus-resident cache in EBR. `ENABLE_ICACHE` toggles its synthesis.

Performance benchmarks:
```txt
benchmark (cycles  instrs  CPI  checksum)
-- Compute --
alu           12754952   1850038   6.89   82
shift         11816780   1700031   6.95   0
mul           12705209   2050038   6.19   124
div           11364096   1240032   9.16   183
branch        21069356   2974936   7.08   64
call          27604492   4100028   6.73   32
-- Memory --
memcpy        6204728   930068   6.67   64
chase         6132492   904386   6.78   136
-- Fetch --
hot           26803281   4000027   6.70   117
cold          65926971   1029530   64.03   239
-- Programs --
bubble_sort   4840578   756032   6.40   77   branch/ld-st
matmul        18850448   2922496   6.45   204   mul/ld-st
crc32         19712046   3075453   6.40   0   shift/xor
prime_count   5231708   521375   10.03   141   div/branch
fir           27808623   4328124   6.42   214   mul/shift
strsearch     10523128   1568299   6.70   200   ld/branch
done
```


#### Memory Bus

`picosoc.v` decodes the single `mem_*` bus by address into parallel *responders*, whose `*_ready`/`*_rdata` are OR/priority-mux'd back to the CPU:

```
mem_ready = (iomem_valid && iomem_ready)        // GPIO etc. (icebreaker.v)
          || cache_ready                         // flash region (icache -> spimemio)
          || ram_ready                           // SPRAM
          || spimemio_cfgreg_sel                 // 0x02000000
          || simpleuart_reg_div_sel              // 0x02000004
          || (simpleuart_reg_dat_sel && !wait);  // 0x02000008
mem_rdata = priority-mux in the same order      
```

- The cache is **one responder on the flash range** (`0x00020000 ≤ addr < 0x02000000`), chosen purely by address — `mem_instr` is never consulted. Flash holds only read-only `.text`/`.rodata`, so every access it sees is a read: no write policy needed. It's really a *(read-only flash) cache*, not strictly an I-cache.
- Note **SPRAM answers in 1 cycle** — `ram_ready <= mem_valid && !mem_ready && addr < 4*MEM_WORDS`, a registered pulse the cycle after the request. 2-cycle cache hit still slower.

#### SoC block diagram — baseline vs with cache

**Baseline (`ENABLE_ICACHE=0`)** — the `mem_*` bus fans out by address; `spimemio` serves the flash region directly.

```latex {cmd=true hide=true latex_zoom=1.7}
\documentclass[tikz,border=8pt]{standalone}
\usepackage{tikz}
\usetikzlibrary{arrows.meta, positioning, fit, backgrounds}
\begin{document}
\begin{tikzpicture}[>=Stealth, semithick, scale=0.82, every node/.style={transform shape},
  blk/.style={draw, rounded corners=2pt, minimum height=0.8cm,
              minimum width=2.0cm, font=\sffamily\scriptsize, align=center},
  arr/.style={->, thick},
  lbl/.style={font=\sffamily\tiny},
  flbl/.style={font=\sffamily\tiny, fill=white, inner sep=1pt}]
  \begin{scope}[on background layer]
  \fill[green!4, rounded corners=6pt] (-2.6, -3.2) rectangle (14.0, 4.0);
  \draw[green!40!black!30, thick, rounded corners=6pt] (-2.6, -3.2) rectangle (14.0, 4.0);
  \node[font=\sffamily\scriptsize\bfseries, green!40!black] at (5.7, 3.75) {icebreaker.v};
  \fill[blue!5, rounded corners=4pt] (-2.2, -2.8) rectangle (10.2, 3.5);
  \draw[blue!40, thick, rounded corners=4pt] (-2.2, -2.8) rectangle (10.2, 3.5);
  \node[font=\sffamily\scriptsize\bfseries, blue!50!black] at (4.0, 3.25) {picosoc.v};
  \end{scope}
  \node[blk, fill=blue!20, minimum height=1.6cm, minimum width=2.4cm] (cpu) at (0, 0.5) {};
  \node[font=\sffamily\scriptsize, align=center] at (0, 0.92) {picorv32\\CPU};
  \node[blk, fill=blue!35, minimum height=0.5cm, minimum width=1.9cm, font=\sffamily\tiny] (rf) at (0, 0.08) {register file (4$\times$ EBR)};
  \node[blk, fill=red!6, minimum width=2.0cm, minimum height=3.8cm] (adec) at (4.0, 0.95) {Address\\Decode};
  \draw[arr] (cpu.east) -- (adec.west) node[midway, above, lbl] {\texttt{mem\_*}};
  \node[blk, fill=blue!22] (spram)  at (8.0, 2.6) {SPRAM\\128\,KB};
  \node[blk, fill=blue!16]  (flash)  at (8.0, 1.5) {spimemio};
  \node[blk, fill=yellow!12](spicfg) at (8.0, 0.5) {UART};
  \node[blk, fill=gray!15, minimum width=2.2cm] (iomem) at (8.0, -0.7) {iomem bus / GPIO};
  \draw[arr] (adec.east |- spram)  -- (spram.west)  node[midway, flbl] {\texttt{< 0x00020000}};
  \draw[arr] (adec.east |- flash)  -- (flash.west)  node[midway, flbl] {\texttt{0x00020000+}};
  \draw[arr] (adec.east |- spicfg) -- (spicfg.west) node[midway, flbl] {\texttt{0x02000004/8}};
  \draw[arr] (adec.east |- iomem)  -- (iomem.west)  node[midway, flbl] {\texttt{>= 0x02000000}};
  \node[blk, fill=gray!15, minimum width=1.6cm] (gpio) at (12.2, -0.7) {LEDs};
  \node[blk, fill=gray!15, minimum width=1.6cm] (sevs) at (12.2, -1.8) {7-seg};
  \draw[arr] (iomem.east) -- ++(0.5, 0) coordinate (iofan);
  \draw[arr] (iofan) |- (gpio.west) node[pos=0.25, above, lbl] {\texttt{0x03..}};
  \draw[arr] (iofan) |- (sevs.west);
  \begin{scope}[on background layer]
  \draw[<-, thick, blue!50] (cpu.south) -- (0, -2.4) -- (9.2, -2.4)
    node[pos=0.5, below, flbl, text=blue!50] {\texttt{mem\_ready / mem\_rdata}};
  \foreach \nd/\xoff in {spram/-0.3, flash/-0.1, spicfg/0.1, iomem/0.3} {
    \draw[thick, blue!50] ([xshift=\xoff cm]\nd.south) -- ([xshift=\xoff cm]\nd.south |- 0,-2.4);
  }
  \end{scope}
  \node[draw, rounded corners=2pt, fill=gray!8, font=\sffamily\tiny, inner sep=3pt, align=center] (bflash) at (11.5, 1.5) {W25Q128\\SPI Flash};
  \node[draw, rounded corners=2pt, fill=gray!8, font=\sffamily\tiny, inner sep=3pt, align=center] (buart) at (11.5, 0.5) {FT2232H\\UART};
  \draw[arr, gray] (flash.east) -- (bflash.west) node[midway, above, lbl] {QSPI};
  \draw[arr, gray] (spicfg.east) -- (buart.west) node[midway, above, lbl] {TX/RX};
\end{tikzpicture}
\end{document}
```

**With the cache (`ENABLE_ICACHE=1`)** — hits return on the same bus, misses fall through to `spimemio` via `spi_*`

```latex {cmd=true hide=true latex_zoom=1.7}
\documentclass[tikz,border=8pt]{standalone}
\usepackage{tikz}
\usetikzlibrary{arrows.meta, positioning, fit, backgrounds}

\begin{document}

\begin{tikzpicture}[>=Stealth, semithick, scale=0.78, every node/.style={transform shape},
  blk/.style={draw, rounded corners=2pt, minimum height=0.8cm,
              minimum width=2.0cm, font=\sffamily\scriptsize, align=center},
  arr/.style={->, thick},
  lbl/.style={font=\sffamily\tiny},
  flbl/.style={font=\sffamily\tiny, fill=white, inner sep=1pt}]

  \begin{scope}[on background layer]
    \fill[green!4, rounded corners=6pt] (-2.6, -3.4) rectangle (14.8, 4.0);
    \draw[green!40!black!30, thick, rounded corners=6pt] (-2.6, -3.4) rectangle (14.8, 4.0);
    \node[font=\sffamily\scriptsize\bfseries, green!40!black] at (6.0, 3.75) {icebreaker.v};

    \fill[blue!5, rounded corners=4pt] (-2.2, -3.0) rectangle (11.0, 3.4);
    \draw[blue!40, thick, rounded corners=4pt] (-2.2, -3.0) rectangle (11.0, 3.4);
    \node[font=\sffamily\scriptsize\bfseries, blue!50!black] at (-0.9, 3.15) {picosoc.v};
  \end{scope}

  \node[blk, fill=blue!20, minimum height=1.6cm, minimum width=2.4cm] (cpu) at (0, 0.5) {};
  \node[font=\sffamily\scriptsize, align=center] at (0, 0.92) {picorv32\\CPU};

  \node[blk, fill=blue!35, minimum height=0.5cm, minimum width=1.9cm,
        font=\sffamily\tiny] (rf) at (0, 0.08) {register file (4$\times$ EBR)};

  \node[blk, fill=red!6, minimum width=1.7cm, minimum height=4.4cm]
        (adec) at (3.4, 0.5) {Address\\Decode};

  \draw[arr] (cpu.east) -- (adec.west)
    node[midway, above, lbl] {\texttt{mem\_*}};

  \node[blk, fill=green!18, minimum height=1.0cm, minimum width=1.9cm,
        draw=green!50!black, thick, dashed]
        (cache) at (6.5, 2.4) {Instr Cache\\{\tiny 256$\times$32 EBR}};

  \node[blk, fill=blue!16]  (flash)  at (9.6, 2.4) {spimemio\\SPI Flash};
  \node[blk, fill=blue!22]  (spram)  at (9.6, 1.0) {SPRAM\\128\,KB};
  \node[blk, fill=yellow!12](spicfg) at (9.6, -0.3) {SPI cfg / UART};
  \node[blk, fill=gray!15, minimum width=2.2cm] (iomem) at (9.6, -1.5) {iomem bus};

  \draw[arr, green!50!black] (adec.east |- cache) -- (cache.west)
    node[pos=0.97, below left=2pt and 2pt, flbl, text=green!50!black, align=center]
    {\texttt{0x0002\_0000..}\\\texttt{0x01FF\_FFFF}};

  \draw[arr] (cache.east) -- (flash.west)
    node[midway, above, lbl] {miss \texttt{(spi\_*)}};

  \draw[arr] (adec.east |- spram) -- (spram.west)
    node[midway, flbl] {\texttt{< 0x20000}};

  \draw[arr] (adec.east |- spicfg) -- (spicfg.west)
    node[midway, flbl] {\texttt{0x0200\_000x}};

  \draw[arr] (adec.east |- iomem) -- (iomem.west)
    node[midway, flbl] {\texttt{[31:24]>0x01}};

  \node[blk, fill=gray!15, minimum width=1.4cm] (gpio) at (13.0, -1.1) {LEDs};
  \node[blk, fill=gray!15, minimum width=1.4cm] (sevs) at (13.0, -2.2) {7-seg};

  \draw[arr] (iomem.east) -- ++(0.4, 0) coordinate (iofan);
  \draw[arr] (iofan) |- (gpio.west)
    node[pos=0.25, above, lbl] {\texttt{0x03..}};
  \draw[arr] (iofan) |- (sevs.west);

  \begin{scope}[on background layer]
    \draw[<-, thick, blue!50] (cpu.south) -- (0, -2.6) -- (10.3, -2.6)
      node[pos=0.5, below, flbl, text=blue!50] {\texttt{mem\_ready / mem\_rdata}};

    \foreach \nd/\xoff in {cache/0, spram/-0.2, spicfg/0, iomem/0.2} {
      \draw[thick, blue!50]
        ([xshift=\xoff cm]\nd.south) -- ([xshift=\xoff cm]\nd.south |- 0,-2.6);
    }
  \end{scope}

  \node[draw, rounded corners=2pt, fill=gray!8, font=\sffamily\tiny,
        inner sep=3pt, align=center]
        (bflash) at (12.7, 2.4) {W25Q128\\Flash};

  \draw[arr, gray] (flash.east) -- (bflash.west)
    node[midway, above, lbl] {QSPI};

\end{tikzpicture}

\end{document}
```

#### Structure

Parametric and read-only (no write policy): `SETS × WAYS × WORDS_PER_LINE` words, set by the params at the top of `icache.v` (`WAYS=1` ⇒ direct-mapped, `WAYS>1` ⇒ set-associative with round-robin replacement). **Currently built 2-way × 4-word × 256-set = 8 KB.** Two EBR-inferred arrays, written *full-width per set* (the whole line, all ways) so yosys never sees a dynamic partial-memory write:

```verilog
reg [DATAW-1:0] data_mem [0:SETS-1];   // per set: all ways' lines   (DATAW = WAYS*WORDS_PER_LINE*32)
reg [TAGMW-1:0] tag_mem  [0:SETS-1];   // per set: all ways' {valid, tag}  (TAGW = 1 + tag bits)
```

The 24-bit flash address `cpu_addr = mem_addr[23:0]` splits parametrically — with `i = log2(SETS)`, `w = log2(WORDS_PER_LINE)`:

```text
 tag [23 : 2+i+w] | index [1+i+w : 2+w] | word [1+w : 2] | byte [1:0]
```

For the current 2-way × 4-word × 256-set build (`i=8, w=2`): `tag [23:12] (12b) | index [11:4] (256 sets) | word [3:2] (4/line) | byte [1:0]`.

For a hit, the `index` selects the set; the `WAYS` stored tags are read and compared **in parallel** against the address `tag`, and a way hits if its valid bit is set and its tag matches. The `word` offset then selects which word of the hit line to return. On a miss the victim way's line is (re)filled from `spimemio` — multi-word lines stream their words sequentially (the cheap data-phase burst), then the CPU is acked.

#### EBR Reset

A post-reset sweep clears every valid bit before the first cache lookup. EBR powers up undefined, so a stale tag could falsely match.

> In sim the uninitialised `tag_mem` reads `X` and the hit-compare is *accidentally* false, so sweep was unneccessary. Hardware runs into issues

#### Cache FSM

Three steady-state states (entered once the reset sweep above finishes). `cpu_ready` and `spi_valid` default to 0 every cycle, so they are single-cycle pulses unless re-asserted.

```latex {cmd=true hide=true latex_zoom=1.7}
\documentclass[tikz,border=8pt]{standalone}
\usepackage{tikz}
\usetikzlibrary{arrows.meta}
\begin{document}
\begin{tikzpicture}[>=Stealth, semithick,
  st/.style={draw, circle, minimum size=1.25cm, font=\sffamily\tiny, inner sep=0.5pt, align=center},
  lbl/.style={font=\sffamily\tiny, inner sep=1.5pt, fill=white, align=center}]

  \node[st, fill=gray!15]  (idle) at (0, 0)     {idle\\(0)};
  \node[st, fill=green!12] (chk)  at (5.0, 0)    {check\\(1)};
  \node[st, fill=blue!15]  (fill) at (5.0, -3.4) {fill\\(2)};

  \draw[->] (-1.8, 0) -- node[lbl, above] {reset} (idle);

  % labels go BETWEEN the coords (edge midpoint); a node after the last coord lands ON it
  \draw[->] (idle) to[bend left=20] node[lbl, above] {cpu\_valid\\\&\,!cpu\_ready} (chk);
  \draw[->] (chk)  to[bend left=20] node[lbl, below] {hit:\\cpu\_ready} (idle);

  \draw[->] (chk) -- node[lbl, right] {miss:\\spi\_valid} (fill);
  \path (fill) edge[->, loop right, looseness=6] node[lbl] {!spi\_ready} (fill);
  \draw[->] (fill) to[bend left=20] node[lbl, above left] {spi\_ready:\\fill line,\\cpu\_ready} (idle);

\end{tikzpicture}
\end{document}
```

- **idle**
  - on a cache request, latch the `index`, `tag` and `word` offset sliced from `cpu_addr` (parametric fields, see Structure above)
    - `req_tag` is compared next cycle in `check`
  - latch the tag + data EBR reads (`tag_q <= tag_mem[idx]`, `data_q <= data_mem[idx]`) — synchronous, so the latch lands next cycle
- **check**
  - compare the address `tag` against all `WAYS` stored tags + valid bits in parallel `(tag_q[way] == {1'b1, req_tag})`
  - On **hit**, select the hitting way and the `word` offset, store the word, ack the CPU `cpu_ready<=1`
  - On **miss**, then kick off a SPI flash read.
- **fill**
  - hold the flash request until `spi_ready` when SPI read is done
  - write the instruction word into cache, then ack CPU and return it

> The `!cpu_ready` term in idle's accept condition drops the one stale cycle right after a hit — when `cpu_ready` and `cpu_valid` are both still high — so the FSM doesn't re-latch the just-finished address as a phantom request.

#### Timing

Signals `cpu_valid`/`cpu_ready` represent `CPU--cache` request/ack
Signals `spi_valid`/`spi_ready` represent `cache--SPI` on a miss

In the **baseline** (no cache), every taken branch/call/return is a **non-sequential** fetch driven straight into `spimemio`. With nothing in the path, `spi_valid`/`spi_ready` (the `spimemio` handshake) are just `mem_valid`/`mem_ready` passed through — **same edges, no offset**. `spimemio` raises `jump`, re-runs the full command + address + dummy + data, and holds ready **low** the whole time — ~**128 system cycles**, not to scale:

```latex {cmd=true hide=true latex_zoom=1.5}
\documentclass[tikz,border=8pt]{standalone}
\usepackage{tikz}
\usepackage{tikz-timing}
\begin{document}
\begin{tikztimingtable}[timing/wscale=1.4, timing/dslope=0.08,
  timing/d/background/.style={fill=blue!6}]
  \texttt{clk}          & 16{C}                                \\
  \texttt{mem\_valid}   & 2L 13H L                             \\
  \texttt{spi\_valid}   & 2L 13H L                             \\
  \texttt{mem\_addr}    & 2Z 13D{target} Z                     \\
  \texttt{spi\_ready}   & 14L H L                              \\
  \texttt{mem\_ready}   & 14L H L                              \\
  \texttt{mem\_rdata}   & 14Z {[timing/d/background/.style={fill=green!8}] D{instr}} Z \\
\end{tikztimingtable}
\end{document}
```

With a cache, the same fetch **hits** in **2 cycles** — `idle` latches + launches the EBR read, `check` compares and raises the *registered* `cpu_ready` (equivalent = `mem_ready`, next cycle):

```latex {cmd=true hide=true latex_zoom=1.5}
\documentclass[tikz,border=8pt]{standalone}
\usepackage{tikz}
\usepackage{tikz-timing}
\begin{document}
\begin{tikztimingtable}[timing/wscale=2, timing/dslope=0.08,
  timing/d/background/.style={fill=blue!6}]
  \texttt{clk}           & 8{C}                            \\
  \texttt{mem\_valid}    & 2L 3H 3L                        \\
  \texttt{spi\_valid}    & 8L                              \\
  \texttt{mem\_addr}     & 2Z 3D{target} 3Z                \\
  \texttt{spi\_ready}    & 8L                              \\
  \texttt{cache\_state}  & 3D{idle} D{check} 4D{idle}      \\
  \texttt{mem\_ready}    & 4L H 3L                         \\
  \texttt{mem\_rdata}    & 4Z {[timing/d/background/.style={fill=green!8}] D{instr}} 3Z \\
\end{tikztimingtable}
\end{document}
```

So **~128 → 2** on every taken branch/call/return.

However,

- **Still 1 cycle slower than SPRAM** (which acks at T+1, below) — purely because `cpu_ready` is *registered* in `check`; `idle` spends a whole cycle just launching the EBR read. That one cycle per fetch is why the plateau is **~6.4 CPI, not ~4**:

```latex {cmd=true hide=true latex_zoom=1.5}
\documentclass[tikz,border=8pt]{standalone}
\usepackage{tikz}
\usepackage{tikz-timing}
\begin{document}
\begin{tikztimingtable}[timing/wscale=2.4, timing/dslope=0.08,
  timing/d/background/.style={fill=blue!6}]
  \texttt{clk}        & 5{C}                       \\
  \texttt{mem\_valid} & L 2H 2L                    \\
  \texttt{ram\_ready} & 2L H 2L                    \\
  \texttt{ram\_rdata} & 2Z {[timing/d/background/.style={fill=green!8}] D{word}} 2Z \\
\end{tikztimingtable}
\end{document}
```

- **`cold` fetches about 1.0x** — straight-line misses come out in order, so `spimemio` streams them exactly as the no-cache baseline did. The full **miss path** = 2-cycle check + the `spimemio` transaction + a 1-cycle fill ack.

#### Further Optimizations

The parametric rewrite (geometry, associativity, multi-word lines, registered outputs) is **shipped** — see Structure above and `icache_tb.v`. Two levers remain open; don't conflate them.

> MODULAR CACHE IS SHIPPED!

##### Hit latency (2 → 1 cycle) — *not built*

The 2-cycle hit costs ~1 cycle vs SPRAM on every fetch (concentrated on branch/jump/load/store; ALU fetches are already hidden by `spimemio` prefetch). The safe fix is **look-ahead** (`LOOKAHEAD_HIT`): pre-read the EBR off picorv32's `mem_la_addr` one cycle early and answer at T+1 with `cpu_ready` still **registered** — needs `mem_la_*` wired through `picosoc`. *Avoid* the combinational hit (`cpu_ready = (state==check) && hit`) — it routes the tag-compare straight into `mem_ready`, which **is** the critical path.

##### Miss behaviour: cache sizing (still open tuning)

Sizing is invisible on our ~100%-hit proxies but is the *only* thing that matters on the worst kernel / a rival cache-buster (see **Competition Strategy**). The geometry is parametric and currently **2-way × 4-word × 256-set**, but the final point isn't committed — and the two knobs cover *different* flanks (they don't substitute):

- **capacity / multi-word lines** → the footprint window (large hot code). Multi-word is the EBR-cheap capacity (fewer tags per word) and rides the shipped fast-read streaming. EBR is surplus; **LC is the ceiling (~88%)** — re-read nextpnr LC after any growth.
- **associativity (`WAYS`)** → conflict/aliasing. A *constructed* stride-aliased attack 100%-misses a direct-mapped cache of **any** size; only ways are a structural fix.

Remaining work is **measurement, not design**: a footprint-sweep proxy (parameterise `bench_interp`'s handler size) to find the capacity knee, and a conflict probe (two hot regions one cache-stride apart) to decide whether `WAYS=2` earns its LC. Size to the measured knee with margin (past a plausible rival cache). Correctness/hit-miss/invalidation across geometries is already covered by `icache_tb.v`.

### Incremental CPI reductions

Smaller wins within the repo

#### Barrel Shifter

`BARREL_SHIFTER=1` shifts finish combinationally (3 cyc) instead of iterating (up to ~14). ~200 LCs. Also a **prerequisite for pipelining** (a fixed-latency Execute stage can't tolerate the shift loop).

Effects are only noticeable with `ENABLE_ICACHE=1` (and `BARREL_SHIFTER=1`):
```
benchmark (cycles  instrs  CPI  checksum)
-- Compute --
alu           12754952   1850038   6.89   82
shift         11254283   1700031   6.62   0
mul           12705209   2050038   6.19   124
div           11124102   1240032   8.97   183
branch        20669364   2974936   6.94   64
call          27604492   4100028   6.73   32
-- Memory --
memcpy        6101478   930068   6.56   64
chase         6082238   904386   6.72   136
-- Fetch --
hot           25803286   4000027   6.45   117
cold          65926971   1029530   64.03   239
-- Programs --
bubble_sort   4755105   756032   6.28   77   branch/ld-st
matmul        18702160   2922496   6.39   204   mul/ld-st
crc32         19703862   3075453   6.40   0   shift/xor
prime_count   5231708   521375   10.03   141   div/branch
fir           27783350   4328124   6.41   214   mul/shift
strsearch     10506752   1568299   6.69   200   ld/branch
done
```

#### **Fast-path load/store**

loads/stores cost 5 cycles because the memory states are entered twice for the generic handshake. Data always targets 1-cycle SPRAM, so a fast path can collapse the second visit, saving a cycle per access with no pipeline machinery.

#### **Radix-4 SRT divider**

`pcpi_div` retires 1 quotient bit/cycle (~32 cyc); radix-4 does 2 bits/cycle (~16) for modest LCs. Helps any divide-bearing benchmark. (Multiply is already DSP-backed — leave it.)

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

### Clock frequency and Critical Path

#### PLL

The board only supplies a raw **12 MHz** crystal on the clock pad. The iCE40's hard **PLL** (`SB_PLL40_PAD`) multiplies that reference up to a faster system clock — currently **~17.25 MHz** (`DIVF`=45), a touch below the ~18.5 MHz $f_{max}$ ceiling. We retune this often, so the live value lives in `DIVF` (`icebreaker.v`) and `F_CLK_HZ` (`benchmarks.h`), not in this prose. Refer to the `SB_PLL40_PAD` code block in `icebreaker.v`:

```verilog
module icebreaker (
	input clk_in, // physical pin input now drives the PLL, instead of system clock directly
  ...
);
...
wire clk;            // system clock, driven by PLL
wire pll_locked;
...
SB_PLL40_PAD #(
    .FEEDBACK_PATH("SIMPLE"),
    .DIVR(4'b0000),       // = 0
    .DIVF(7'b0101101),    // = 45
    .DIVQ(3'b101),        // = 5
    .FILTER_RANGE(3'b001)
) pll (
    .PACKAGEPIN(clk_in),  // 12 MHz crystal pad (pin 35) as input
    .PLLOUTCORE(clk),     // go through PLL for synthesized system clock out
    .RESETB(1'b1), .BYPASS(1'b0),
    .LOCK(pll_locked)
);
...
```

**The frequency** is set by three dividers (run `icepll -i 12 -o <ideal_freq>` to get valid values for a new target):

$$
f_{out} = f_{ref}\,\frac{DIVF+1}{(DIVR+1)\,2^{DIVQ}} = 12\,\frac{46}{32} = 17.25\text{ MHz}
$$

Physical constraints bound our divider values: the **VCO** ($f_{ref}\frac{DIVF+1}{DIVR+1}$) must stay in **533–1066 MHz**, the phase-detector input ≥ 10 MHz, and the **PLL output floor is 16 MHz**. Lock takes ≤ 50 µs (the reason for the reset-until-`pll_locked` gate).

> Note SCK (= clk/2) rises in lockstep, so a flash fetch still costs the same *number of cycles* but less wall-clock time — the PLL helps even fetch-bound code.

#### Setting a custom clock rate

You request `icepll` for an `<ideal_freq>`. It then computes accounting for physcial constraints, and returns the **nearest achievable** `<target_freq>` which is is what we'll use.

1. **Compute divisors** — `icepll -i 12 -o <ideal_freq>` prints `DIVR`/`DIVF`/`DIVQ`/`FILTER_RANGE` **and** the actual `<target_freq>` it landed on (ask for 20 → get 19.875).
2. **`icebreaker.v`** — paste those four divider values into the `SB_PLL40_PAD #( ... )` params.
3. **`benchmarks.h`** — set `F_CLK_HZ` to `<target_freq>` (in Hz). `firmware.c` derives the UART divisor (`= round(F_CLK_HZ/BAUD)`) and `benchmarks.c` derives wall-clock time from it
4. **`PicoSoC Makefile`** — `nextpnr-ice40 --freq <target_freq>` (the P&R target).
5. **`PicoSoC Makefile`** — `icetime -d up5k -c <target_freq>` (the timing-check constraint).

> Then rebuild and read `icebreaker.rpt`: if its `Total path delay` line reports a frequency **below** `<target_freq>`, timing failed — lower the target (or shorten the critical path) and retry.
> The final timing netlist will report something like `Creating timing netlist.. Timing estimate: 53.63 ns (18.64 MHz)... Checking 53.33 ns (18.75 MHz) clock constraint: FAILED.` so you'll need to lower the target frequency

#### Clock Ceilings

The achievable clock is the **minimum** of several independent limits:

| Limit | Freq | Binding when… |
| --- | ---: | --- |
| **Logic critical path** | **~18.5 MHz (now)** | **currently** — see Critical Path below |
| DSP `MULT16×16` (pipeline bypassed) | 50 MHz | once logic < 20 ns; `pcpi_fast_mul` maps to the DSP, clocked at `clk` |
| SPRAM read/write | 70 MHz | once the DSP is pipelined or fast-mul dropped |
| EBR / regfile | 150 MHz | never binding here |
| Global clock net | 185 MHz | never binding here |

**How to get critical path frequency:**
- check `icebreaker.rpt` (icetime): `Total path delay: 54.18 ns (18.46 MHz)`
- nextpnr (print to terminal after synth) also prints `Max frequency for clock ...: 19.94 MHz (PASS at 18.38 MHz)` to stdout.

**We currently clock 17.25 MHz (`DIVF`=45) against the ~18.5 MHz critical-path ceiling** — so there's a sliver of unused PLL headroom (Step 0 in Critical Path below), and the logic path is the wall above that.
- **Shorten the critical path** to lift the ceiling, then the next bottleneck is the DSP at 50 MHz. The lever is *not* `TWO_CYCLE_ALU` (the ALU isn't on the path) — see the [Critical Path](#critical-path) diagnosis and plan below.

The payoff is large because the PLL speeds *everything* linearly in wall-clock (including flash, since SCK = clk/2). But it's **gated on critical-path work**, which is why the cache (works today) comes first and `T_c` tweaks are enablers.

##### Critical Path

The actual critical path (`icebreaker.rpt`, post-route — **54.05 ns / 18.5 MHz, 15 logic levels, routing-dominated**: the LocalMux/Span4 hops ≈1.1 ns each dwarf the ~0.7 ns LUTs):

```text
mem_addr[29]  (DFF, inside cpu)                                  1.5 ns  ┐ FRONT
  → iomem_addr  (crosses out to icebreaker.v)                           │ ~21 ns
  → iomem_ready / mem_ready OR-mux  (GPIO decode round-trips back in)   │
  → mem_xfer   (= mem_valid & mem_ready)                        20.8 ns  ┘
  → mem_done → ldmem/stmem control                                      ┐ BACK
       (mem_do_prefetch, instr_sb, instr_sll ×3 …)                      │ ~33 ns
  → reg_op1.CE  (+ the reg_op1 + decoded_imm adder)             54.0 ns  ┘
```

Two semantically distinct halves, and the split is the whole story:

- **Front (`mem_addr` → `mem_xfer`, ~21 ns)** — the SoC address-decode + `mem_ready` OR-mux. The worst path *starts* at the **GPIO (`iomem`) decode, which lives in `icebreaker.v`, *outside* `picosoc`** — so `mem_addr` round-trips out to the top level and back, and that cross-module hop is a big slice of the (routing-dominated) delay. The depth is the **combinational** peripheral selects (`iomem_valid`, `cfgreg_sel`, `uart_*_sel`); `cache_ready`/`ram_ready` are already FFs and cost nothing on this path.
- **Back (`mem_xfer` → `reg_op1.CE`, ~33 ns)** — *not* generic decode: it's the **load/store address calc**. In `ldmem`/`stmem`, `reg_op1 <= reg_op1 + decoded_imm` is gated by `(!mem_do_prefetch || mem_done)`, and `mem_done` is combinational off `mem_xfer` off `mem_ready`. The chain is literally "*the prefetch just finished → so compute the data address this cycle*." The `instr_*` terms + carry are that enable cone plus the address adder.

**Three facts that drive the plan:**

1. It is **one** combinational FF→FF chain, so shortening *any* segment shortens the whole — front and back are not separate paths.
2. The ALU/comparator is **nowhere** on it → `TWO_CYCLE_ALU`/`TWO_CYCLE_COMPARE` add a cycle for *nothing*.
3. **Routing-dominated** → cutting logic *levels* helps less than moving logic onto FFs (so the router places it freely) and killing the cross-module round-trip.

###### Plan — shorten it

Re-read `Total path delay` after **every** step: the wall moves, and PnR placement is nondeterministic, so the frequencies below are *estimates*, not promises.

> **Verify correctness, not just fmax.** A structural change touches the memory handshake, so after each step rebuild the firmware and `make icebsim`: the testbench's trap detector must stay silent and the benchmark checksums must still match the known-good column above (`bench_matmul` = 204, etc.). For the Step 1 decode rework, write a focused `mem_ready`/look-ahead testbench (drive `mem_la_*`/`mem_valid` sequences, check `mem_ready`/`mem_rdata` route to the same responder as the baseline) before trusting the board. fmax (`icetime`) and correctness (`icebsim` + tb) are *both* gates.

**Step 0 — free, zero-RTL (do first; they also recalibrate the true ceiling):**

- **Seed sweep** (`nextpnr --seed`) — *already tried* (placement is nondeterministic; a few seeds buy a few %).
- **Register retiming** (`synth_ice40 -retime`) — let abc rebalance FFs across the cloud. Untried, free.
- **Spend the PLL-to-closure margin** — we clock **17.25 MHz** against an ~18.5 MHz (icetime) / ~19.9 MHz (nextpnr) ceiling. If that gap is spendable, nudge `DIVF` toward ~18.4 (free ~7%) — but re-confirm it still P&Rs (we may sit at 17.25 *because* it closes reliably).

**Step 1 — the structural clock win: get the address decode out of the `mem_ready` chain.** Two routes to the same goal — a shallow `mem_ready` = OR of flip-flops, with the peripheral address-compares (and the combinational GPIO decode that currently round-trips through `icebreaker.v`) off the path. **Step 1a is preferred**; 1b is the lighter fallback if 1a's wiring proves too invasive.

**Build it behind a default-off knob** (same convention as `ENABLE_ICACHE`/`FLASH_INIT_MODE`): a param plumbed `icebreaker.v` → `picosoc.v` (e.g. `LOOKAHEAD_DECODE`) that defaults to today's combinational decode, so the verified baseline stays byte-identical and the new path is A/B-testable for fmax vs. CPI from one line in `icebreaker.v`.

- **Step 1a — Look-ahead pre-decode (*preferred*).** Route picorv32's `mem_la_addr`/`mem_la_read` into `picosoc` (already driven by the core; `picosoc.v` just doesn't wire them) — a tiny **shared prerequisite**, then two *independent* pieces build on it. They touch different modules, target different metrics, and carry different risk, so do them **one at a time and verify after each** (don't bundle — a bundled gain/regression is un-attributable):

  - **1a-i — decode fix (✅ done; the modest fmax win).** Off the look-ahead address, **register a one-hot region-select a cycle early**, so `mem_ready` becomes an FF-select with **no wait state** (the look-ahead address is valid the cycle before `mem_valid` → transactions stay 1-cycle, no double-write hazard). Shortens the critical-path front — *estimated* ~21 → ~5 ns; **measured ~20 → ~13 ns**, a real but small win because the back half dominates (full results + corrected conclusion below). Lives in `picosoc.v`; lower risk. *(The "no wait state" claim rests on the timing assumption below — verified in `lookahead_tb.v`.)*
  - **1a-ii — cache 1-cycle hit (a CPI bonus, do second, separate change).** The same `mem_la_*` wiring lets `icache` **pre-read its EBR off `mem_la_addr`** and answer a hot-loop hit in 1 cycle instead of 2 (~6.4 → ~5.x CPI), with `cpu_ready` still *registered* — **not** the rejected combinational hit. Lives in `icache.v`; riskier (it changes the hot-loop hit FSM — the path the I-cache *freeze* log was about). After landing it, **re-measure fmax** to prove the pre-read didn't claw the path back (it shouldn't — `cpu_ready` stays an FF — but verify).

  Cost: a few new ports through `picosoc` (prereq + 1a-i), plus `icache` for 1a-ii. This is what the look-ahead interface is *for*.

> **Load-bearing assumption (verify, don't assume):** the "no wait state" claim requires `mem_la_read`/`mem_la_write` to lead `mem_valid` by *exactly one cycle for every transaction*, so the region-select registered off `mem_la_addr` matches the address `mem_valid` actually presents next cycle. This holds for `COMPRESSED_ISA=0` (no RVC prefetched-high-word replay), but **confirm it in sim** — a mismatch routes a transaction to the wrong responder. Also reproduce the existing decode's **region overlap/priority exactly**: `iomem_valid` (`addr[31:24] > 0x01`) covers *both* `0x02` (cfgreg/UART, exact-match selects) and `0x03` (GPIO), so the pre-decode must keep the same precedence (exact `0x0200_000x` selects beat the broad `> 0x01` iomem catch-all).

- **Step 1b — Register the cold responders (*lighter fallback*).** Make cfgreg/UART return a *registered* `ready`/`rdata` (GPIO already does, via its `!iomem_ready` guard), dropping their compares off the chain. ~0 CPI — the kernels never touch those peripherals (`time_benchmark` brackets only `fn()`). **Caveat — it's reads, not writes:** the added wait state makes the registered `ready` land a cycle *after* select, so a *read* must **latch the responder's `rdata` at select-time** — `simpleuart` self-clears `recv_buf_valid` on `reg_dat_re`, so a late capture returns `~0` (empty) instead of the byte. (Writes are already safe: GPIO's `!iomem_ready` guard, the UART's `send_bitcnt` guard, idempotent cfgreg — there's no double-write.) Only the interactive menu hits these (the scored kernels touch no peripheral), but it must still work. This whole class of gotcha is *why 1a is preferred*: pre-registering only the select keeps peripherals 1-cycle, so nothing changes latency.

~~Estimate: front ~21 → ~5 ns ⇒ ~38 ns ≈ **~26 MHz**.~~ **Superseded by measurement — see below.**

###### ✅ Implemented + measured (1a-i)

Built behind `LOOKAHEAD_DECODE` (default 0; plumbed `icebreaker.v` → `picosoc.v`): wire the core's `mem_la_addr`/`mem_la_read`/`mem_la_write` into `picosoc` and register the one-hot peripheral region-select a cycle early off `mem_la_addr`. Files touched: `picosoc.v` (ports + the generate-selected decode), `icebreaker.v` (param). **1a-ii (cache 1-cycle hit) is NOT done** — deferred pending the conclusion below. Knob-off stays byte-identical to baseline.

**Correctness** — `picosoc/lookahead_tb.v` runs the SoC with the knob on and asserts, on **every** `mem_valid` cycle, that the registered region-select equals the *true* combinational decode of the address actually presented (this is the direct test of the load-bearing 1-cycle-lead assumption + the region overlap/priority). Result: boots, no trap, **793,681 checks all matched**, with cfgreg (`S`) / UART-echo / GPIO exercised. Knob-off rebuild = baseline `SIM PASS`. Build it by swapping `lookahead_tb.v` for `icebreaker_tb.v` in the `iverilog` line; on-board, still confirm `bench_matmul`=204 etc.

**fmax** (`nextpnr --seed 1..5 --freq 40`, the ICACHE + qddr config, seed-matched A/B):

| | baseline | 1a-i | Δ |
| --- | ---: | ---: | ---: |
| mean of 5 seeds | 19.33 MHz | **20.54 MHz** | **+6.3%** |
| best seed | 19.63 MHz | **21.41 MHz** | **+9.1%** |

1a-i wins **every** seed, at **zero CPI cost** — but it is a **~+7% trim, not the ~26 MHz first estimated.** Why the estimate was wrong (post-route `icetime`/`nextpnr` evidence):

- The front *did* shrink — `mem_addr → mem_xfer` went ~20 → ~13 ns, and the path no longer *starts* at `mem_addr` (the `mem_addr→decode` cone is gone; it now starts at the `iomem_ready` FF). So the mechanism works.
- **But it is one continuous FF→FF chain**, and the dominant part is the **back half** — `mem_xfer` → `mem_do_*` → decoder → `instr_sll` cascade → `reg_op1.CE`, **~33–42 ns**, entirely inside `picorv32`. 1a-i doesn't touch it.
- The front was never ~21 ns of *removable* delay: ~13 ns of it (the cross-module `iomem_ready` arrival + the `mem_ready` OR + the `mem_xfer` fan-out) survives, and the back dominates the total either way.
- **Seed placement noise on the back (~±4 MHz) exceeds the front saving (~+1.2 MHz mean)** — so a single seed can read as ~0 (seed 1: +1.4%) while the 5-seed mean is a clean +6.3%. The mean is the honest figure.

**Corrected conclusion.** 1a-i is correct, free, and a modest repeatable win worth keeping — but it does **not** unlock the clock. The wall is the in-core **back half**, which needs CPU surgery (**fast-path load/store** to collapse the double memory-state visit, or **pipelining**), not SoC-decode work. Step 0 (retiming, spend the PLL margin) plausibly stacks on top of 1a-i for a bit more. **Recommendation:** keep `LOOKAHEAD_DECODE` as a default-off, A/B-testable knob; treat the back half as the next real target.

**Step 2 — free experiments on the back half (flip the param, rebuild, read the delay):**

- **`CATCH_MISALIGN=0`** — drops the `reg_op1[1:0]` misalign check that lives in the load/store control (plausibly near this path). Transparent for *any* compiled rv32im (it never misaligns), and saves LCs. Unverified on-path → an experiment, not a promise.
- **`ENABLE_IRQ=0`** — the picosoc IRQ inputs are tied to 0 and compiled C can't emit the custom IRQ ops, so the IRQ FSM is dead weight that thins `cpu_state` and saves ~100 LCs. *Lower confidence:* it's a feature removal and we can't prove the unknown eval firmware won't arm IRQs — treat as optional.

**The wall (out of scope for a clean retime):** the back half is picorv32's `mem_done` → load/store address-calc reaction (~33 ns ≈ 30 MHz). Past it means restructuring how the multicycle FSM reacts to `mem_done` — **fast-path load/store** (collapse the double memory-state visit) or full **pipelining**. CPU surgery, a separate project. And the **DSP hard-caps ~50 MHz** regardless, so *~30 MHz clean + an eventual pipeline* is the realistic ladder.

**Rejected (with the report as the evidence):**

- **Register `mem_ready` wholesale** — cuts the chain but taxes *every* fetch/load/store with a wait state (~1.2× CPI) and undoes the cache win. Step 1 gets most of the gain without the tax, because peripherals are free to slow down but memory is not.
- **Combinational cache hit** (drive `cpu_ready` combinationally on a hit) — feeds `cache_ready` straight *into the `mem_ready` front-half mux that is the bottleneck*; it lengthens the binding path. Use the look-ahead 1-cycle hit (1a) instead.
- **`TWO_CYCLE_ALU`/`TWO_CYCLE_COMPARE`** — the ALU is nowhere on the path; pure CPI loss.

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
