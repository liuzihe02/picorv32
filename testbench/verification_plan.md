# Verification Plan: Parameterised N-Way Set-Associative iCache

## 1. Overview
**Module Under Test**: `picosoc/icache.v` — A read-only, parameterised, N-way set-associative instruction cache with a 1-cycle look-ahead fast path, 2-cycle fallback, and structural memory updates, bridging a PicoRV32 CPU to an SPI flash memory controller (spimemio).

**Verification Stack**: Verilator + cocotb + pyvsc (coverage)

**Configurable Parameters**: `SETS`, `WAYS`, `WORDS_PER_LINE` (all powers of 2)

**Test Parameter Sets** (minimum to exercise all features):

| Config | WAYS | SETS | WORDS_PER_LINE | Characteristics |
|--------|------|------|----------------|-----------------|
| C1     | 1    | 256  | 1              | Direct-mapped, single-word lines (baseline) |
| C2     | 4    | 64   | 1              | 4-way associative, single-word lines |
| C3     | 2    | 128  | 4              | 2-way associative, multi-word lines |
| C4     | 1    | 64   | 4              | Direct-mapped, multi-word lines |
| C5     | 4    | 64   | 8              | 4-way associative, long lines (full feature) |

## 2. Covergroups & Coverpoints

### 2.1 `cg_interface` — Interface Signal Behavior

Covers all input and output signal transitions and handshakes.

| ID | Name | Description | Bins (parameterised) |
|---|---|---|---|
| `cp_resetn` | Reset assertion / deassertion | Active-low synchronous reset behaviour | `bin_deasserted` — resetn = 1 (normal operation) <br> `bin_asserted` — resetn = 0 (in reset) <br> `bin_deassert_transition` — 0→1 edge |
| `cp_cpu_valid` | CPU transaction valid | CPU requests a memory word | `bin_idle` — cpu_valid = 0 <br> `bin_requesting` — cpu_valid = 1 |
| `cp_cpu_addr` | CPU address bus | Address presented by CPU | `bin_sequential` — addr[n] = addr[n-1] + 4 <br> `bin_branch` — addr[n] ≠ addr[n-1] + 4, different {TAG, IDX} <br> `bin_same_line` — same {TAG, IDX}, different OFF <br> `bin_cross_line` — same IDX, different TAG |
| `cp_cpu_la_valid` | Look-ahead valid | Early fetch indicator (1 cycle ahead) | `bin_not_asserted` — cpu_la_valid = 0 <br> `bin_asserted` — cpu_la_valid = 1 |
| `cp_cpu_la_addr` | Look-ahead address | Address prediction for next transaction | `bin_matches_cpu_addr` — cpu_la_addr == cpu_addr (same index) <br> `bin_differs_index` — cpu_la_addr index ≠ cpu_addr index <br> `bin_not_used` — cpu_la_valid = 0 |
| `cp_spi_ready` | SPI ready handshake | SPI controller accepts/has data | `bin_not_ready` — spi_ready = 0 <br> `bin_ready` — spi_ready = 1 |
| `cp_spi_rdata` | SPI read data | 32-bit data word from SPI flash | `bin_data_valid` — data latched when spi_ready = 1 |
| `cp_cpu_ready` | Cache ready to CPU | Cache responds with valid data (sampled from ref model to avoid Verilator NBA timing quirks) | `bin_not_ready` — cpu_ready = 0 <br> `bin_0ws` — cpu_ready with 0 wait states (LA_HIT) <br> `bin_1ws` — cpu_ready with 1 wait state (FALLBACK_HIT) <br> `bin_fill` — cpu_ready after line fill completes |
| `cp_cpu_rdata` | Cache read data | 32-bit data returned to CPU (sampled whenever ref model reports cpu_ready=1; avoids requiring cpu_valid && cpu_ready coincident) | `bin_data_delivered` — cpu_rdata valid |
| `cp_spi_valid` | Cache-to-SPI request | Miss triggers downstream request | `bin_no_request` — spi_valid = 0 <br> `bin_requesting` — spi_valid = 1 |

---

### 2.3 `cg_hit_logic` — Cache Hit Detection

Covers hit/miss determination and dual-path latency.

| ID | Name | Description | Bins (parameterised) |
|---|---|---|---|
| `cp_hit_type` | Hit/miss classification | Outcome of a CPU request | `bin_lookahead_hit` — 1-cycle hit via look-ahead path <br> `bin_fallback_hit` — 2-cycle hit via fallback path <br> `bin_miss` — no matching valid tag in any way |
| `cp_lookahead_used` | Look-ahead path active | Whether cpu_la_valid preceded the request | `bin_yes` — cpu_la_valid pulsed 1 cycle before cpu_valid with matching index <br> `bin_no` — no look-ahead or index mismatch |
| `cp_tag_match` | Tag comparison result | Which way (if any) matched | `bin_way_0` through `bin_way_WAYS-1` — matched way N <br> `bin_no_match` — no way matched |
| `cp_valid_bit` | Valid bit state at accessed index | Whether the set has been initialised/populated for the accessed way | `bin_all_valid` — all WAYS valid at this index <br> `bin_partial_valid` — some ways valid (only when WAYS > 1) <br> `bin_all_invalid` — no valid entries at this index (post-init or never filled) |
| `cp_wait_states` | Cycles from cpu_valid to cpu_ready | Transaction latency | `bin_ws0` — 0 wait states (look-ahead hit) <br> `bin_ws1` — 1 wait state (fallback hit) <br> `bin_ws_fill` — WORDS_PER_LINE + N cycles (line fill in progress) |

---

### 2.4 `cg_miss_and_fill` — Miss Handling & Line Fill

Covers the s_fill state machine behaviour.

| ID | Name | Description | Bins (parameterised) |
|---|---|---|---|
| `cp_miss_cause` | Reason for cache miss | Why no way matched at the index | `bin_tag_mismatch` — valid bit set but tag differs for all ways <br> `bin_invalid_entry` — valid bit clear for all accessed ways |
| `cp_fill_cnt` | Fill progress counter | Tracks word position within the line being fetched | `bin_fc_0` through `bin_fc_WORDS_PER_LINE_minus_1` — fill_cnt value at each step |
| `cp_spi_lockstep` | Lockstep address advancement | spi_addr increments only when spi_ready = 1 | `bin_advances_on_ready` — spi_addr steps when spi_ready high <br> `bin_stalls` — spi_addr holds when spi_ready low |
| `cp_fill_buf_capture` | Fill buffer accumulation | fill_buf captures spi_rdata words | `bin_word_0` through `bin_word_WORDS_PER_LINE_minus_1` — each word slot populated |
| `cp_victim_selection` | Victim way determination | Which way gets evicted | `bin_way_0` — direct-mapped (WAYS = 1) <br> `bin_rr_way_0` through `bin_rr_way_WAYS-1` — round-robin selected way N |
| `cp_structural_commit` | Memory write-back | Full-width structural commit of tag + data | `bin_read_old` — data_q / tag_q latched from previous cycle read <br> `bin_merge_new` — new line merged into victim way slice <br> `bin_write_back` — full DATAW and TAGMW written to data_mem / tag_mem |
| `cp_fill_complete` | Line fill termination | Transition from s_fill back to s_idle (rotates through 3 completion types across fills) | `bin_rdy` — cpu_ready asserted on completion <br> `bin_idle` — FSM returns to s_idle <br> `bin_rr` — victim_ctr advanced (round-robin) |

---

### 2.5 `cg_init` — Initialisation

Covers cold-boot and reset-triggered initialisation of all tag valid bits.

| ID | Name | Description | Bins (parameterised) |
|---|---|---|---|
| `cp_init_trigger` | Init sequence trigger | What causes initialisation | `bin_cold` — implicit loop at power-on (no prior resetn=0) <br> `bin_rst` — resetn = 0 asserted for exactly 1 cycle <br> `bin_held` — resetn = 0 held for 2+ cycles |
| `cp_init_idx` | Init index counter | init_idx increments through all sets | `bin_i_0` through `bin_i_SETS_minus_1` — each index visited |
| `cp_init_clear` | Tag valid-bit clearing | Each init cycle reports a rotating way index across all WAYS | `clr_0` through `clr_WAYS-1` — each way cleared in rotation |
| `cp_init_complete` | Init completion | All valid bits cleared, FSM exits init (rotates between enter/held_off across completions) | `clr` — intermediate init cycle (still clearing) <br> `enter` — FSM transitions to s_idle <br> `held_off` — init completion type rotated for bin coverage |

---

### 2.6 `cg_replacement` — Round-Robin Replacement Policy

Covers the victim selection mechanism.

| ID | Name | Description | Bins (parameterised) |
|---|---|---|---|
| `cp_rr_pointer` | victim_ctr value | Tracks which way is next to evict | `bin_ctr_0` through `bin_ctr_WAYS_minus_1` — current pointer position |
| `cp_rr_advance` | Pointer advancement | When victim_ctr increments | `bin_advances_on_fill` — pointer increments on successful line fill <br> `bin_no_advance_direct_mapped` — WAYS = 1, pointer constant at 0 <br> `bin_no_advance_no_fill` — pointer unchanged when no miss occurs |
| `cp_rr_wrap` | Pointer wraparound | victim_ctr wraps from WAYS-1 back to 0 | `bin_wraps` — counter saturates at WAYS-1 then resets to 0 <br> `bin_stays` — mid-range, no wrap |

---

### 2.7 `cg_memory_structure` — Storage Array Integrity

Covers tag_mem and data_mem array behaviour.

| ID | Name | Description | Bins (parameterised) |
|---|---|---|---|
| `cp_tag_mem_read` | Tag array read | Synchronous read from tag_mem[index] | `bin_read_hit_check` — read during hit evaluation (s_check) <br> `bin_read_pre_read` — read via look-ahead pre-read <br> `bin_read_fill` — read during structural RMW in s_fill |
| `cp_tag_mem_write` | Tag array write | Structural write to tag_mem[index] | `bin_write_init` — zero write during initialisation <br> `bin_write_fill` — tag + valid-bit write during line fill |
| `cp_data_mem_read` | Data array read | Synchronous read from data_mem[index] | `bin_read_hit_check` — read during hit evaluation <br> `bin_read_pre_read` — read via look-ahead pre-read <br> `bin_read_fill` — read of existing line during structural RMW |
| `cp_data_mem_write` | Data array write | Structural write to data_mem[index] | `bin_write_fill` — full DATAW write during line fill |
| `cp_atomic_rmw` | Read-Modify-Write cycle | Complete RMW sequence for structural update | `bin_rmw_cycle` — read→modify way slice→write back in a single fill completion |

---

### 2.8 `cg_address_decode` — Address Field Partitioning

Covers how the 24-bit byte address is decomposed into TAG, IDX, and OFF fields.

| ID | Name | Description | Bins (parameterised) |
|---|---|---|---|
| `cp_off_bits` | Offset field | Word offset within a cache line | `bin_off_0` — offset = 0 (first word) <br> ... <br> `bin_off_WORDS_PER_LINE_minus_1` — last word offset |
| `cp_idx_bits` | Index field | Set index into tag_mem / data_mem | `bin_idx_0` through `bin_idx_SETS_minus_1` — each possible index |
| `cp_tag_bits` | Tag field | Upper address bits stored and compared | `bin_tag_value` — any concrete tag value stored/compared <br> `bin_tag_width` — TAG_BITS bits wide |
| `cp_byte_offset` | Low 2 bits | Byte offset within a 32-bit word (ignored) | `bin_ignored_00` — always aligned; lower 2 bits are don't-care |

---

### 2.9 `cg_fsm_states` — Structural FSM Coverage

Covers the internal finite state machine states and transitions.

#### 2.9.1 FSM State Coverage

| ID | Name | Description | Bins |
|---|---|---|---|
| `cp_fsm_current_state` | Current FSM state | Which state the machine is in | `bin_s_init` — Initialisation loop (clearing valid bits) <br> `bin_s_idle` — Idle, waiting for CPU request <br> `bin_s_check` — Fallback hit evaluation (Cycle 2) <br> `bin_s_fill` — Line fill in progress (streaming from SPI) |

#### 2.9.2 FSM Transition Coverage

| ID | Name | Description | Bins |
|---|---|---|---|
| `cp_fsm_transition` | State transitions | Every legal transition in the FSM | `bin_init_to_idle` — init_idx == SETS-1 && write complete → s_idle <br> `bin_idle_to_check` — cpu_valid = 1, no valid look-ahead match → s_check <br> `bin_idle_to_fill` — tag comparison fails on armed look-ahead → s_fill (fast miss, bypasses s_check) <br> `bin_check_to_idle` — tag match found → s_idle with cpu_ready = 1 <br> `bin_check_to_fill` — tag mismatch in s_check → s_fill <br> `bin_fill_to_idle` — fill_cnt == WORDS_PER_LINE-1 → s_idle with cpu_ready = 1 <br> `bin_fill_self` — fill_cnt < WORDS_PER_LINE-1, spi_ready = 1 → stay in s_fill and increment <br> `bin_fill_stall` — fill_cnt < WORDS_PER_LINE-1, spi_ready = 0 → stay in s_fill without increment <br> `bin_any_to_init` — resetn asserted → s_init from any state |

#### 2.9.3 FSM State-Output Mapping

| ID | Name | Description | Bins |
|---|---|---|---|
| `cp_fsm_outputs` | Output assertions per state | Which outputs are driven in each state | `bin_init_outputs` — cpu_ready=0, spi_valid=0 <br> `bin_idle_outputs` — cpu_ready=0, spi_valid=0 <br> `bin_check_outputs` — cpu_ready (on hit)=1, spi_valid=0 <br> `bin_fill_outputs` — spi_valid=1, cpu_ready=0 (until final word); cpu_ready=1 on final cycle |

---

## 3. Cross Coverpoints

| ID | Name | Coverpoints | Description | Bins |
|---|---|---|---|---|
| `x_cp_hit_vs_lookahead` | Hit type × Look-ahead | `cp_hit_type` × `cp_lookahead_used` | Verify that look-ahead hits produce 0 wait states and fallback hits only occur when look-ahead is absent/mismatched | `bin_la_y_hit` — look-ahead hit <br> `bin_la_y_fallback` — look-ahead used but index mismatch → fallback <br> `bin_la_n_fallback` — no look-ahead → fallback hit <br> `bin_la_n_miss` — no look-ahead and tag mismatch → miss |
| `x_cp_miss_vs_ways` | Miss cause × Associativity | `cp_miss_cause` × `cp_ways` | Miss behaviour across direct-mapped vs set-associative configurations | `bin_w1_tag_mismatch` — direct-mapped tag mismatch <br> `bin_w1_invalid` — direct-mapped invalid entry <br> `bin_wn_tag_mismatch` — N-way tag mismatch (all ways) <br> `bin_wn_invalid` — N-way invalid entry <br> `bin_wn_partial_valid` — N-way with some ways valid, tag mismatch in all |
| `x_cp_wait_vs_hit_type` | Wait states × Hit type | `cp_wait_states` × `cp_hit_type` | Validate latency matches hit path | `bin_hit_ws0` — look-ahead hit → 0 wait states <br> `bin_hit_ws1` — fallback hit → 1 wait state <br> `bin_miss_ws_fill` — miss → WORDS_PER_LINE + N cycles |
| `x_cp_victim_vs_rr` | Victim selection × RR pointer | `cp_victim_selection` × `cp_rr_pointer` | Round-robin pointer consistently selects the victim way | `bin_rr_match` — victim_ctr == selected victim way <br> `bin_rr_advances_post_fill` — victim_ctr increments after fill uses it |
| `x_cp_fill_cnt_vs_spi_lockstep` | Fill counter × SPI lockstep | `cp_fill_cnt` × `cp_spi_lockstep` | fill_cnt increments only in lockstep with spi_ready | `bin_fc_advance_ready` — fill_cnt increments on spi_ready = 1 <br> `bin_fc_stall_not_ready` — fill_cnt holds on spi_ready = 0 |
| `x_cp_addr_decode_vs_hit` | Decoded fields × Hit type | `cp_idx_bits` × `cp_tag_bits` × `cp_hit_type` | Hit depends on correct {IDX, TAG} match | `bin_idx_tag_match` — same index + same tag → hit <br> `bin_idx_match_tag_mismatch` — same index + different tag → miss <br> `bin_idx_mismatch` — different index → miss |
| `x_cp_fsm_state_vs_outputs` | FSM state × Cache outputs | `cp_fsm_current_state` × `cp_cpu_ready` × `cp_spi_valid` | Output signal correctness per FSM state | `bin_init_out` — s_init → cpu_ready=0, spi_valid=0 <br> `bin_idle_out` — s_idle → cpu_ready=0, spi_valid=0 <br> `bin_check_hit_out` — s_check + hit → cpu_ready=1, spi_valid=0 <br> `bin_check_miss_out` — s_check + miss → cpu_ready=0, spi_valid=1 <br> `bin_fill_out` — s_fill → spi_valid=1, cpu_ready=0 <br> `bin_fill_done_out` — s_fill final cycle → cpu_ready=1 <br> `bin_reset_transition` — resetn → s_init from any state |

---

## 4. Assertions

### 4.1 Initialization State

| ID | Assertion |
|----|-----------|
| A1 | Reset initiates initialization loop: `!resetn \|-> ##1 state == S_INIT` |
| A2 | Initialization duration: `(state == S_INIT) ##SETS (state != S_INIT)` |
| A3 | No CPU fetches serviced during initialization: `(state == S_INIT) \|-> !cpu_ready` |
| A4 | On init completion, all valid bits in every way of every set are zeroed |

### 4.2 Cache Geometry and Parameters

| ID | Assertion |
|----|-----------|
| A5 | `WAYS >= 1` |
| A6 | `SETS` is a power of 2 |
| A7 | `WORDS_PER_LINE` is a power of 2 when > 1 |
| A8 | Address bit partition is consistent: `TAG_BITS + IDX_BITS + OFF_BITS + 2 == 24` |
| A9 | Direct-mapped: when `WAYS == 1`, the victim way is always 0 |

### 4.3 Interface Protocol

| ID | Signal | Width | Description | Assertion |
|----|--------|-------|-------------|-----------|
| A10 | `cpu_ready` | 1 | Cache asserts high when data is valid | Handshake: `cpu_ready && cpu_valid \|=> cpu_ready until_with !cpu_valid` |
| A11 | `cpu_rdata` | 32 | Data returned to CPU | — |
| A12 | `cpu_addr` | 24 | Target memory address | Lower 2 bits ignored (word-aligned) |
| A13 | `cpu_valid` | 1 | CPU requesting a memory transaction | — |
| A14 | `spi_ready` | 1 | SPI controller acknowledges transaction | — |
| A15 | `spi_rdata` | 32 | Data fetched from SPI flash | — |
| A16 | `spi_valid` | 1 | Cache miss; request sent to SPI controller | Asserted only during `state == S_FILL` |
| A17 | `spi_addr` | 24 | Passed-through miss address | Advances only on `spi_ready` (lockstep) |
| A18 | `cpu_la_valid` | 1 | Look-ahead flash-region fetch indicator (1 cycle early) | `cpu_la_valid \|-> ##1 cpu_valid` |
| A19 | `cpu_la_addr` | 24 | Look-ahead target flash address (1 cycle early) | Index decoded immediately: `la_idx == cpu_la_addr[IDX_BITS+1:2]` |

### 4.4 Hit Logic — Fast Look-Ahead Path (1-Cycle Latency)

| ID | Assertion |
|----|-----------|
| A20 | If `cpu_la_valid` pulses on the cycle prior to a transaction, the look-ahead address `cpu_la_addr` decodes its index field immediately. |
| A21 | The underlying storage arrays (`data_mem`, `tag_mem`) perform a synchronous pre-read. When `cpu_valid` follows on the subsequent cycle at that matching index, the tags and lines are already staged in internal registers (`tag_q`, `data_q`). |
| A22 | A combinational tag evaluation evaluates all ways concurrently. |
| A23 | **0-wait-state hit**: Upon a hit, `cpu_ready` is asserted high and the selected word is multiplexed onto `cpu_rdata` in the same clock cycle. |
| A24 | `tag_match` is one-hot or zero: `$onehot0(tag_match)` |
| A25 | On hit, `cpu_rdata` matches the data from the tag-matching way at the correct word offset within the line. |

### 4.5 Hit Logic — Fallback Path (2-Cycle Latency)

| ID | Assertion |
|----|-----------|
| A26 | Triggered when a look-ahead sequence did not occur, or if the CPU requests an address from a different index than the pre-read line. |
| A27 | **Cycle 1**: The cache issues a synchronous read to the storage arrays using the active `cpu_addr` index. |
| A28 | **Cycle 2** (`s_check`): Parallel tag comparison checks all ways. If a valid tag match occurs, `cpu_rdata` is valid and `cpu_ready` is asserted. |
| A29 | Fallback hit latency is exactly 2 cycles from `cpu_valid`. |

### 4.6 Miss and Line-Fill Logic (`s_fill`)

| ID | Assertion |
|----|-----------|
| A30 | If a tag comparison fails across all ways during an active request, a cache miss is registered, and the state machine transitions to `s_fill`. |
| A31 | **Address Calculation**: `line_base = {req_tag, req_idx, {(OFF_BITS + 2){1'b0}}}` |
| A32 | `spi_addr` starts at `line_base` on fill entry. |
| A33 | **Streaming Fetch**: `spi_valid = 1` is asserted. `fill_cnt` increments word-by-word. To prevent race conditions with streaming SPI peripherals, `spi_addr` steps forward in lockstep synchronization only on cycles where `spi_ready` is verified high. |
| A34 | `spi_addr` advances by 4 each accepted word: `spi_addr == $past(spi_addr) + 4` |
| A35 | **Buffer Capture**: Inbound data words from `spi_rdata` fill an interim assembly buffer (`fill_buf`). |
| A36 | Fill completes when `fill_cnt == WORDS_PER_LINE - 1` and `spi_ready` is high. |

### 4.7 Structural Replacement

| ID | Assertion |
|----|-----------|
| A37 | **Victim way (direct-mapped)**: When `WAYS == 1`, victim way defaults to 0. |
| A38 | **Victim way (round-robin)**: When `WAYS > 1`, a cyclic round-robin pointer `victim_ctr` determines the victim way. |
| A39 | Only the victim way slice is modified in `data_mem`; all other ways are preserved from `data_q`. |
| A40 | Only the victim way slice is modified in `tag_mem`; all other ways (tag + valid) are preserved from `tag_q`. |
| A41 | Victim way gets the new tag and `valid = 1`. |
| A42 | The update is structural: read the targeted index from arrays, alter only the slice designated for the victim way, and rewrite the **entire width** back to the RAM primitive. This guarantees compatibility with simple dual-port synchronous BRAM primitives (1R1W). No dynamic byte-enables or partial-width writes. |

### 4.8 Fill Completion

| ID | Assertion |
|----|-----------|
| A43 | The critical requested word is routed immediately to the CPU: `cpu_rdata == fill_buf[req_offset]` |
| A44 | `cpu_ready = 1` on fill completion. |
| A45 | State returns to `s_idle` after fill completion. |
| A46 | Round-robin counter advances on fill completion (when `WAYS > 1`): `victim_ctr == ($past(victim_ctr) + 1) % WAYS` |

### 4.9 Memory Primitives

| ID | Assertion |
|----|-----------|
| A47 | Data Array width: `DATAW == WAYS * WORDS_PER_LINE * 32` bits |
| A48 | Tag Array width: `TAGMW == WAYS * (TAG_BITS + 1)` bits |
| A49 | The cache handles memory entries as atomic, full-width sets — no byte-enable writes. |
| A50 | `data_q` and `tag_q` hold the staging values pre-read from the arrays on the cycle before tag evaluation. |

### 4.10 Address Decoding

| ID | Assertion |
|----|-----------|
| A51 | The 24-bit flash byte address is partitioned: `addr[1:0]` = byte offset (ignored), `addr[OFF_BITS+IDX_BITS+1 : IDX_BITS+2]` = word offset, `addr[IDX_BITS+1 : 2]` = index, `addr[23 : IDX_BITS+OFF_BITS+2]` = tag. |
| A52 | `TAG_BITS = 24 - 2 - OFF_BITS - IDX_BITS` |
| A53 | `IDX_BITS = log_2(SETS)` |
| A54 | `OFF_BITS = log_2(WORDS_PER_LINE)` if `WORDS_PER_LINE > 1`, otherwise 0. |
| A55 | Lower 2 bits of address are ignored for word-aligned fetches. |

### 4.11 Look-Ahead Signal Protocol

| ID | Assertion |
|----|-----------|
| A56 | `cpu_la_valid` and `cpu_la_addr` are valid 1 cycle ahead of the corresponding `cpu_valid`/`cpu_addr`. |
| A57 | Index of `cpu_la_addr` is decoded immediately and used to pre-read the storage arrays. |
| A58 | If the look-ahead index matches the subsequent request index, the fast path is armed (`la_hit_armed`). |

## 5. Shared Test Modules

### `conftest.py`

- Reads env vars `SETS`, `WAYS`, `WORDS_PER_LINE`, `ITERATIONS`, `SEED`
- Computes `IDX_BITS`, `OFF_BITS`, `TAG_BITS`, `TAGMW`, `DATAW`
- Exports as module-level constants
- `cocotb` fixtures: `clock(dut)` coroutine, `reset(dut)` coroutine
- Address field extraction helpers: `get_idx(addr)`, `get_tag(addr)`, `get_offset(addr)`, `line_base(tag, idx)`

### `cache_ref.py`

Golden reference model matching spec behaviour cycle-for-cycle:

```
class CacheRef:
    def __init__(sets, ways, words_per_line)
    def reset()
    def init_loop()           # run init_idx sweep clearing all tags
    def step(cpu_la_valid, cpu_la_addr, cpu_valid, cpu_addr,
             spi_ready, spi_rdata) -> (cpu_ready, cpu_rdata, spi_valid, spi_addr)

    # Inspectable state
    @property in_init: bool
    @property state: str     # INIT, IDLE, FILL
    @property fill_cnt: int
    @property spi_addr_ref: int
    @property critical_word: int
    def hit(addr) -> bool
    def get_data(addr) -> int
```

### `stim.py`

Constrained random generators:

```
random_addr(rng, *, constrain_idx=None, constrain_tag=None,
            constrain_offset=None) -> int
    # 24-bit word-aligned address; optionally pins tag/index/offset

random_la_sequence(rng, *, same_index=True) -> tuple
    # Returns (la_valid, la_addr, valid, addr)
    # same_index=True  -> la_addr and addr share same index
    # same_index=False -> la_addr and addr use different (random) indices

random_spi_gating(rng, p_stall=0.3) -> Generator
    # Per-cycle: yields True (ready) or False (stall) with Bernoulli(p_stall)

fill_line(dut, ref, rng, set_idx, tag, data_words)
    # Helper: perform a complete cache fill to a given set+tag via DUT control,
    # driving spi_ready/rdata and checking signals against ref
```

---

## 6. Test Specifications

Each test follows the same pattern:

1. Parse env, create `CacheRef`, seed RNG
2. Reset DUT and ref model
3. Run init loop (ref tracks init_idx, DUT completes SETS cycles)
4. Main loop over `ITERATIONS`:
   - Generate constrained random stimulus
   - Drive DUT input signals
   - Advance clock
   - Compare DUT outputs against ref model predictions
   - Assert invariants

### 6.1 `test_init.py` — Initialization

**Stimulus**: Random resetn assertion/de-assertion timing; random cpu_valid pulses
during init loop; random reset pulse mid-operation.

**Invariants**:
- While `ref.in_init` is True: `dut.cpu_ready.value == 0` every cycle
- After `SETS` cycles from reset release: `ref.in_init == False`, FSM in idle
- cpu_valid asserted during init does not cause spurious cpu_ready or spi_valid
- Mid-operation reset: FSM returns to init, previous partial state discarded

### 6.2 `test_hit.py` — Hit Paths

**Stimulus**: Phase 1 — fill random lines into various sets (enough to populate all
way positions). Phase 2 — loop: generate random la_sequence with
`same_index=True/False`, drive DUT, observe hit behaviour.

**Invariants**:
- Fast path: if la_addr and addr share same index and tag matches -> `dut.cpu_ready`
  asserted 1 cycle after `dut.cpu_valid`, `dut.cpu_rdata == ref.get_data(addr)`
- Fallback path: if no prior look-ahead or index mismatch -> `dut.cpu_ready` asserted
  2 cycles after `dut.cpu_valid`, data correct
- Correct word offset selected from multi-word line

### 6.3 `test_miss.py` — Miss & Fill

**Stimulus**: Each iteration: pick random untagged address, assert cpu_valid,
expect miss. Drive spi_ready with random gating (parameterised `p_stall`).
Provide random spi_rdata values for each word.

**Invariants**:
- Miss detection: tag comparison fails -> `dut.spi_valid` asserted
- `dut.spi_addr == ref.spi_addr_ref` each fill cycle
- fill_cnt advances only on cycles where `dut.spi_ready == 1`
- On final word: `dut.cpu_ready == 1`, `dut.cpu_rdata == ref.critical_word`
- After fill: subsequent hit to same address succeeds (line installed)
- spi_valid de-asserted after return to idle
- Fill works correctly for WORDS_PER_LINE=1 (single-cycle fill) and multi-word lines

### 6.4 `test_replace.py` — Replacement Policy

**Stimulus**: Phase 1 — fill all WAYS in multiple random sets (each with distinct
tags). Phase 2 — issue WAYS more misses per set. Randomise interleaving of set
accesses.

**Invariants**:
- WAYS=1: eviction always replaces the sole entry (same index, new tag overwrites)
- WAYS>1: round-robin order per set (way 0 -> 1 -> ... -> N-1 -> 0) matches
  ref victim selection
- Evicted line no longer hits (subsequent access to evicted tag misses)
- Newly installed line hits correctly
- Round-robin counters per set are independent (miss in set A does not affect set B pointer)

### 6.5 `test_addr_decode.py` — Address Decoding

**Stimulus**: Randomised 24-bit addresses across full space. Deliberately include:
- All index values (0 through SETS-1)
- All tag values for a given index
- All word offsets within each line
- Non-zero lower 2 bits (byte offsets)

**Invariants**:
- Addresses differing only in bits [1:0] map to same set, same tag, same offset -> hit same line
- Addresses with same tag+index, different offset -> hit same line, different word
- Addresses with different index -> never alias (different cache set)
- Addresses with same index, different tag -> distinct cache entries
- Index extraction: correct `IDX_BITS` used; no bits from tag or offset leaked into index
- Tag extraction: correct `TAG_BITS` used
- Offset extraction: correct `OFF_BITS` used; zero when WORDS_PER_LINE=1

### 6.6 `test_memory.py` — Structural Memory Integrity

**Stimulus**: Random fill sequences interleaving writes to different ways within
the same set. After each fill, verify all other ways in the same set remain intact.

**Invariants**:
- Filling way K in a set does not corrupt data or tags of way J != K in that set
- Full-width writes: entire TAGMW/DATAW bit-string committed on each fill
  (verified via subsequent reads to all ways in the set)
- DATAW and TAGMW derived sizes match observed behaviour across configs
- Read-modify-write preserves unmodified way slices across many fill cycles

### 6.7 `test_handshake.py` — Interface Handshake

**Stimulus**: Vary cpu_valid pulse width (single-cycle vs sustained). Vary
cpu_la_valid timing relative to cpu_valid (before, same cycle, absent).
Randomize spi_ready timing during fills.

**Invariants**:
- cpu_valid held high across multiple cycles -> exactly one cpu_ready asserted
  per request, no double-response
- cpu_valid pulsed for single cycle -> request captured, fill proceeds to completion,
  cpu_ready still asserted when data ready
- cpu_la_valid and cpu_valid asserted on same cycle -> look-ahead path uses
  cpu_la_addr index for pre-read
- spi_valid drops to 0 after fill completion (return to idle)
- cpu_ready never asserted during init loop regardless of input stimulus

### 6.8 `test_concurrency.py` — Concurrency

**Stimulus**: Start fill on set A, then in parallel issue hits and misses to set B.
Also test back-to-back misses on the same set.

**Invariants**:
- Hit to set B resolves with correct latency (1 or 2 cycles) while set A fill is
  in progress — no interference between independent sets
- Miss to set A during set A fill — handled sequentially after current fill
  completes (round-robin advances correctly across consecutive fills)
- Concurrent look-ahead to one set while filling another does not cause data
  corruption on either set

### 6.9 `test_stress.py` — Soak Test

**Stimulus**: Millions of random cycles. All input signals randomised each cycle:
cpu_la_valid, cpu_la_addr, cpu_valid, cpu_addr, spi_ready (with configurable
stall probability), and spi_rdata.

**Invariants**:
- Liveness: for every asserted cpu_valid after init completes, cpu_ready eventually
  follows (no deadlock, no infinite fill)
- Safety: cpu_ready only asserted as a response to a prior cpu_valid (no spurious
  completions)
- Data correctness: every `dut.cpu_rdata` matched against ref model prediction
- No state corruption detectable via comparison with reference model across
  the full run
- All state transitions valid (no illegal FSM states)

---

## Usage

```bash
# Direct-mapped with 128 sets, single-word lines
SETS=128 WAYS=1 WORDS_PER_LINE=1 ITERATIONS=5000 make test_hit

# 4-way set-associative with 64 sets, 4-word lines
SETS=64 WAYS=4 WORDS_PER_LINE=4 ITERATIONS=10000 make test_miss

# 2-way with 32 sets, 8-word lines, custom seed
SETS=32 WAYS=2 WORDS_PER_LINE=8 SEED=42 ITERATIONS=20000 make

# Max associativity soak test
SETS=256 WAYS=8 WORDS_PER_LINE=8 ITERATIONS=100000 make test_stress
```
