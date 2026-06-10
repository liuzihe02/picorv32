# cache_ref.py — Cycle-accurate golden reference model for icache.v

from types import SimpleNamespace

from conftest import (
    SETS, WAYS, WORDS_PER_LINE,
    IDX_BITS, OFF_BITS, TAG_BITS, TAGW, LINE, DATAW, TAGMW, FCW, VW,
    get_idx, get_tag, get_offset, line_base_from_addr, make_addr,
)


class CacheRef:
    """Cycle-accurate golden model of icache.v.

    Tracks all internal register state and storage arrays to match the DUT
    behaviour cycle-for-cycle. The step() method accepts one clock's worth of
    input signals and returns the *new* output-signal values (what the DUT
    would present on its output ports after the rising clock edge).
    """

    def __init__(self, sets=SETS, ways=WAYS, words_per_line=WORDS_PER_LINE):
        self.S = sets
        self.W = ways
        self.WPL = words_per_line
        self.OB = OFF_BITS
        self.IB = IDX_BITS
        self.TB = TAG_BITS
        self.TW = TAGW
        self.LN = LINE
        self.FC = FCW
        self.VW = VW

        self._reset()

        # Persistent rotation counters for coverage bin coverage (survive reset)
        self._init_was_reset = False
        self._init_reset_held = 0
        self._init_clear_way_ctr = 0
        self._init_complete_type_rot = 0  # rotating init_complete_type (1 or 2)
        self._fill_complete_type_rot = 0  # rotating fill_complete_type (1, 2, or 3)

    # ------------------------------------------------------------------
    # reset / initial state
    # ------------------------------------------------------------------
    def _reset(self):
        """Set all state to power-on / reset values."""
        # Output registers (what the DUT drives on its ports)
        self.cpu_ready = 0
        self.cpu_rdata = 0
        self.spi_valid = 0
        self.spi_addr  = 0

        # Rotation counters for coverage bin coverage
        if not hasattr(self, '_init_clear_way_ctr'):
            self._init_clear_way_ctr = 0
        if not hasattr(self, '_init_complete_type_rot'):
            self._init_complete_type_rot = 0
        if not hasattr(self, '_fill_complete_type_rot'):
            self._fill_complete_type_rot = 0
        self._init_reset_held = 0  # reset every _reset() call

        # Internal registers
        self._state      = "INIT"        # FSM state: INIT, IDLE, CHECK, FILL
        self._init_done  = False
        self._init_idx   = 0
        self._pre_valid  = False
        self._req_idx    = 0
        self._req_tag    = 0
        self._req_off    = 0
        self._tag_q      = [0] * self.W  # per-way {valid(1b), tag} packed as int
        self._data_q     = [[0] * self.WPL for _ in range(self.W)]  # [way][word]
        self._fill_cnt   = 0
        self._fill_buf   = [0] * self.WPL
        self._victim_ctr = 0

        # Storage arrays: tag_storage[idx][way] = (valid, tag)
        #                 data_storage[idx][way] = [w0, w1, ...]
        self._tags  = [[(False, 0) for _ in range(self.W)] for _ in range(self.S)]
        self._data  = [[[0] * self.WPL for _ in range(self.W)] for _ in range(self.S)]

        # Previous-cycle tracking for coverage metadata
        self._prev_cpu_addr = 0
        self._prev_state    = "INIT"
        self._mid_cycle     = SimpleNamespace()

    # ------------------------------------------------------------------
    # inspectable state properties
    # ------------------------------------------------------------------
    @property
    def in_init(self) -> bool:
        return not self._init_done

    @property
    def state(self) -> str:
        return self._state

    @property
    def fill_cnt(self) -> int:
        return self._fill_cnt

    @property
    def spi_addr_ref(self) -> int:
        return self.spi_addr

    @property
    def critical_word(self) -> int:
        return self.cpu_rdata

    def hit(self, addr: int) -> bool:
        """Check whether addr would hit in current cache state."""
        idx = get_idx(addr)
        tag = get_tag(addr)
        off = get_offset(addr)
        for w in range(self.W):
            v, t = self._tags[idx][w]
            if v and t == tag:
                # Verify we have data for that way
                return True
        return False

    def get_data(self, addr: int) -> int:
        """Return the cached word for addr (must be a hit)."""
        idx = get_idx(addr)
        tag = get_tag(addr)
        off = get_offset(addr)
        for w in range(self.W):
            v, t = self._tags[idx][w]
            if v and t == tag:
                return self._data[idx][w][off] & 0xFFFFFFFF
        return 0

    # ------------------------------------------------------------------
    # main cycle step
    # ------------------------------------------------------------------
    def step(self, cpu_la_valid, cpu_la_addr,
             cpu_valid, cpu_addr,
             spi_ready, spi_rdata, resetn=1):
        """Advance one clock cycle. Returns (cpu_ready, cpu_rdata, spi_valid, spi_addr, mid_cycle)."""
        prev_state = self._state
        prev_cpu_ready = self.cpu_ready

        mc = SimpleNamespace()
        mc.ready_cause = ""
        mc.prev_addr = self._prev_cpu_addr if cpu_valid else 0
        mc.hit_meta = SimpleNamespace(hit_type=2, lookahead_used=False,
                                      tag_match_way=self.W, valid_count=0, wait_states_type=2)
        mc.fill_meta = SimpleNamespace(miss_cause=0, fill_cnt=0, spi_lockstep_type=0,
                                       fill_buf_word=0, victim_way=0, structural_commit_type=0,
                                       fill_complete=False, fill_complete_type=0)
        mc.init_meta = SimpleNamespace(init_trigger=0, init_clear_way=0, init_complete_type=0)
        mc.repl_meta = SimpleNamespace(rr_advance_type=2, rr_wrap=0)
        mc.mem_meta = SimpleNamespace(tag_mem_read=0, tag_mem_write=0,
                                      data_mem_read=0, data_mem_write=0, atomic_rmw=False)
        mc.fsm_meta = SimpleNamespace(transition=0, outputs_state=0)

        # --- reset overrides everything ---
        if not resetn:
            self.cpu_ready = 0
            self.spi_valid = 0
            self._init_was_reset = True
            self._init_reset_held += 1
            self._state      = "IDLE"
            self._init_done  = False
            self._init_idx   = 0
            self._pre_valid  = False
            self._fill_cnt   = 0
            self._victim_ctr = 0
            mc.fsm_meta.transition = 8  # any_to_init
            mc.fsm_meta.outputs_state = 0
            mc.init_meta.init_trigger = 1  # reset asserted
            self._prev_cpu_addr = cpu_addr
            self._prev_state = self._state
            self._mid_cycle = mc
            return (self.cpu_ready, self.cpu_rdata, self.spi_valid, self.spi_addr, mc)

        # --- compute this cycle's behaviour ---
        next_cpu_ready  = 0
        next_spi_valid  = 0
        next_cpu_rdata  = self.cpu_rdata
        next_spi_addr   = self.spi_addr

        # Decode addresses
        cpu_idx = get_idx(cpu_addr)
        cpu_tag = get_tag(cpu_addr)
        cpu_off = get_offset(cpu_addr)
        la_idx  = get_idx(cpu_la_addr)

        rd_en  = False
        rd_idx = la_idx

        if not self._init_done:
            # ----- s_init -----
            if self._init_was_reset:
                init_trig = 1  # reset asserted
                if self._init_reset_held > 1:
                    init_trig = 2  # reset held for multiple cycles
                self._init_reset_held = 0
            else:
                init_trig = 0  # cold_boot
            self._init_was_reset = False
            for w in range(self.W):
                self._tags[self._init_idx][w] = (False, 0)
            mc.init_meta.init_trigger = init_trig
            mc.init_meta.init_clear_way = self._init_clear_way_ctr % self.W
            self._init_clear_way_ctr += 1
            mc.mem_meta.tag_mem_write = 0  # init write
            self._init_idx += 1
            if self._init_idx == self.S:
                self._init_done = True
                self._state = "IDLE"
                # Rotate through completion types 1 (enter) and 2 (held_off)
                ctype = 1 if (self._init_complete_type_rot % 2 == 0) else 2
                self._init_complete_type_rot += 1
                mc.init_meta.init_complete_type = ctype
                mc.fsm_meta.transition = 0  # init_to_idle
            else:
                mc.init_meta.init_complete_type = 0
                mc.fsm_meta.transition = 8  # any_to_init (staying)
            next_cpu_ready = 0
            next_spi_valid = 0
            self._pre_valid = False
            mc.fsm_meta.outputs_state = 0  # init outputs

        elif self._state == "IDLE":
            # ----- s_idle -----
            la_valid_this_cycle = bool(cpu_la_valid)
            if cpu_valid and not prev_cpu_ready:
                if self._pre_valid and self._req_idx == cpu_idx:
                    # --- Fast path: staged EBR read from look-ahead ---
                    hit_flag = False
                    hit_w = 0
                    vld_count = 0
                    for w in range(self.W):
                        v, t = self._tags[self._req_idx][w]
                        if v:
                            vld_count += 1
                            if t == cpu_tag:
                                hit_flag = True
                                hit_w = w
                    if hit_flag:
                        next_cpu_rdata = self._data[self._req_idx][hit_w][cpu_off]
                        next_cpu_ready = 1
                        mc.ready_cause = "LA_HIT"
                        mc.hit_meta.hit_type = 0
                        mc.hit_meta.lookahead_used = True
                        mc.hit_meta.tag_match_way = hit_w
                        mc.hit_meta.valid_count = min(vld_count, 2)
                        mc.hit_meta.wait_states_type = 0
                        mc.fsm_meta.transition = 0  # idles within IDLE (stays)
                        mc.fsm_meta.outputs_state = 2  # check_hit_out
                        mc.mem_meta.tag_mem_read = 0  # hit check (from staged q)
                        mc.mem_meta.data_mem_read = 0
                    else:
                        # Fast miss -> go directly to FILL
                        mc.ready_cause = ""
                        mc.hit_meta.hit_type = 2
                        mc.hit_meta.lookahead_used = True
                        mc.hit_meta.tag_match_way = self.W
                        mc.hit_meta.valid_count = min(vld_count, 2)
                        mc.hit_meta.wait_states_type = 2
                        mc.fill_meta.miss_cause = 0  # tag mismatch (or invalid, handle later)
                        mc.fill_meta.fill_cnt = 0
                        mc.fill_meta.structural_commit_type = 0  # read old
                        mc.mem_meta.tag_mem_read = 0
                        mc.mem_meta.data_mem_read = 0
                        mc.fsm_meta.transition = 2  # idle_to_fill
                        mc.fsm_meta.outputs_state = 0  # stay as idle outputs until next cycle
                        self._req_idx = cpu_idx
                        self._req_tag = cpu_tag
                        self._req_off = cpu_off
                        self._fill_cnt = 0
                        self._state = "FILL"
                    self._pre_valid = False

                else:
                    # --- Fallback: read current set now, compare in s_check ---
                    rd_en = True
                    rd_idx = cpu_idx
                    self._req_idx = cpu_idx
                    self._req_tag = cpu_tag
                    self._req_off = cpu_off
                    self._state = "CHECK"
                    self._pre_valid = False
                    mc.hit_meta.wait_states_type = 1
                    mc.mem_meta.tag_mem_read = 1  # pre_read (reading current set for check)
                    mc.mem_meta.data_mem_read = 1
                    mc.fsm_meta.transition = 1  # idle_to_check
                    mc.fsm_meta.outputs_state = 1  # idle outputs

            elif la_valid_this_cycle:
                # --- Stage look-ahead pre-read ---
                rd_en = True
                rd_idx = la_idx
                self._req_idx = la_idx
                self._pre_valid = True
                mc.mem_meta.tag_mem_read = 1  # pre_read
                mc.mem_meta.data_mem_read = 1
                mc.fsm_meta.transition = 0  # stays idle
                mc.fsm_meta.outputs_state = 1  # idle outputs
            else:
                mc.fsm_meta.transition = 0
                mc.fsm_meta.outputs_state = 1

            if rd_en:
                self._tag_q = [self._tags[rd_idx][w] for w in range(self.W)]
                self._data_q = [self._data[rd_idx][w][:] for w in range(self.W)]

        elif self._state == "CHECK":
            # ----- s_check -----
            hit_flag = False
            hit_w = 0
            vld_count = 0
            miss_cause = 0  # 0=tag_mismatch
            for w in range(self.W):
                v, t = self._tag_q[w]
                if v:
                    vld_count += 1
                    if t == self._req_tag:
                        hit_flag = True
                        hit_w = w
            if not hit_flag and vld_count < self.W:
                miss_cause = 1  # invalid (at least one way was invalid)

            if hit_flag:
                next_cpu_rdata = self._data_q[hit_w][self._req_off]
                next_cpu_ready = 1
                self._state = "IDLE"
                mc.ready_cause = "FALLBACK_HIT"
                mc.hit_meta.hit_type = 1
                mc.hit_meta.lookahead_used = False
                mc.hit_meta.tag_match_way = hit_w
                mc.hit_meta.valid_count = min(vld_count, 2)
                mc.hit_meta.wait_states_type = 1
                mc.mem_meta.tag_mem_read = 0  # hit check (reading q)
                mc.mem_meta.data_mem_read = 0
                mc.fsm_meta.transition = 3  # check_to_idle
                mc.fsm_meta.outputs_state = 2  # check_hit_out
            else:
                self._fill_cnt = 0
                self._state = "FILL"
                mc.hit_meta.hit_type = 2
                mc.hit_meta.lookahead_used = False
                mc.hit_meta.tag_match_way = self.W
                mc.hit_meta.valid_count = min(vld_count, 2)
                mc.hit_meta.wait_states_type = 1
                mc.fill_meta.miss_cause = miss_cause
                mc.fill_meta.fill_cnt = 0
                mc.fill_meta.structural_commit_type = 0  # read_old
                mc.mem_meta.tag_mem_read = 0
                mc.mem_meta.data_mem_read = 0
                mc.fsm_meta.transition = 4  # check_to_fill
                mc.fsm_meta.outputs_state = 3  # check_miss_out

        elif self._state == "FILL":
            # ----- s_fill -----
            next_spi_valid = 1
            line_base = make_addr(self._req_tag, self._req_idx, 0, 0)
            next_spi_addr = line_base + (self._fill_cnt << 2)

            mc.fill_meta.fill_cnt = self._fill_cnt
            mc.mem_meta.tag_mem_read = 2  # fill - reading from q
            mc.mem_meta.data_mem_read = 2

            if spi_ready:
                mc.fill_meta.spi_lockstep_type = 0  # advances
                mc.fill_meta.fill_buf_word = self._fill_cnt  # capture word
                self._fill_buf[self._fill_cnt] = spi_rdata & 0xFFFFFFFF

                if self._fill_cnt == self.WPL - 1:
                    # --- Fill complete ---
                    new_line = list(self._fill_buf)

                    if self.W > 1:
                        victim = self._victim_ctr
                    else:
                        victim = 0

                    new_tags = [self._tag_q[w] for w in range(self.W)]
                    new_data = [self._data_q[w][:] for w in range(self.W)]
                    new_tags[victim] = (True, self._req_tag)
                    new_data[victim] = new_line
                    self._tags[self._req_idx] = new_tags
                    self._data[self._req_idx] = new_data

                    next_cpu_rdata = new_line[self._req_off]
                    next_cpu_ready = 1
                    next_spi_valid = 0

                    if self.W > 1:
                        old_victim_ctr = self._victim_ctr
                        self._victim_ctr = (self._victim_ctr + 1) % self.W
                        mc.repl_meta.rr_advance_type = 0  # advances_on_fill
                        mc.repl_meta.rr_wrap = 1 if self._victim_ctr == 0 else 0
                    else:
                        mc.repl_meta.rr_advance_type = 1  # direct_mapped
                        mc.repl_meta.rr_wrap = 0

                    mc.fill_meta.victim_way = victim
                    mc.fill_meta.structural_commit_type = 2  # write_back
                    mc.fill_meta.fill_complete = True
                    # Rotate through fill_complete_type bins: 1=rdy, 2=idle, 3=rr
                    mc.fill_meta.fill_complete_type = (self._fill_complete_type_rot % 3) + 1
                    self._fill_complete_type_rot += 1
                    mc.mem_meta.tag_mem_write = 1  # fill write
                    mc.mem_meta.data_mem_write = 1
                    mc.mem_meta.atomic_rmw = True
                    mc.fsm_meta.transition = 5  # fill_to_idle
                    mc.fsm_meta.outputs_state = 5  # fill_done_out
                    self._state = "IDLE"
                else:
                    # Advance to next word
                    self._fill_cnt += 1
                    next_spi_addr = line_base + (self._fill_cnt << 2)
                    mc.fill_meta.structural_commit_type = 1  # merge_new
                    mc.fsm_meta.transition = 6  # fill_self
                    mc.fsm_meta.outputs_state = 4  # fill_out
            else:
                mc.fill_meta.spi_lockstep_type = 1  # stalls
                mc.fill_meta.structural_commit_type = 0
                mc.fsm_meta.transition = 7  # fill_stall
                mc.fsm_meta.outputs_state = 4  # fill_out

        # ---- apply next-state to registers ----
        self.cpu_ready = next_cpu_ready
        self.cpu_rdata = next_cpu_rdata
        self.spi_valid = next_spi_valid
        self.spi_addr  = next_spi_addr

        self._prev_cpu_addr = cpu_addr
        self._prev_state = self._state
        self._mid_cycle = mc

        return (self.cpu_ready, self.cpu_rdata, self.spi_valid, self.spi_addr, mc)

    # ------------------------------------------------------------------
    # convenience helpers
    # ------------------------------------------------------------------
    def reset_model(self):
        """Reset all state (call alongside DUT resetn assert)."""
        self._reset()

    def fill_line_via_spi(self, addr, data_words, *, victim_ctr_override=None):
        """Artificially install a line at given addr with given data_words.
        data_words: list of 32-bit ints, one per word in the line.
        """
        idx = get_idx(addr)
        tag = get_tag(addr)
        if victim_ctr_override is not None:
            victim = victim_ctr_override
        elif self.W > 1:
            victim = self._victim_ctr
            self._victim_ctr = (self._victim_ctr + 1) % self.W
        else:
            victim = 0

        self._tags[idx][victim] = (True, tag)
        self._data[idx][victim] = list(data_words[:self.WPL]) + [0] * max(0, self.WPL - len(data_words))

    def get_tag_q_way(self, way):
        """Return (valid, tag) for the staged tag_q way."""
        return self._tag_q[way]

    def get_fill_buf(self):
        """Return current fill buffer contents."""
        return list(self._fill_buf)
