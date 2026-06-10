# coverage.py — pyvsc functional coverage model for icache verification

import atexit
import contextlib
import io
import json
import os
import sys
import time
import traceback
import weakref
import vsc
from conftest import (
    SETS, WAYS, WORDS_PER_LINE,
    IDX_BITS, OFF_BITS, TAG_BITS,
    get_idx, get_tag, get_offset,
)


_COVERAGE_ENABLED = os.environ.get("COVERAGE", "1") != "0"
_COVERAGE_XML = os.environ.get("COVERAGE_XML", "coverage.xml")
_COVERAGE_JSON = os.environ.get("COVERAGE_JSON", "coverage.json")
_COVERAGE_DIR = os.path.dirname(os.path.abspath(__file__))

def _cov_path(filename):
    """Resolve output path: absolute if not already absolute, relative to this file."""
    if os.path.isabs(filename):
        return filename
    return os.path.join(_COVERAGE_DIR, filename)


def _ts_tag():
    return time.strftime("%Y%m%d_%H%M%S") + f"_{int(time.time() * 1e6) % 1000000:06d}"


def _mk_cg(name):
    """Create a covergroup instance and return it directly."""
    return vsc.covergroup()


# ===========================================================================
# 2.1 cg_interface — Interface Signal Behavior
# ===========================================================================
class cg_interface(object):

    def __init__(self):
        # Define coverpoints
        self.cp_resetn = vsc.coverpoint(
            lambda: self._resetn,
            bins={"bin_deasserted": vsc.bin(1), "bin_asserted": vsc.bin(0)},
        )
        self.cp_cpu_valid = vsc.coverpoint(
            lambda: self._cpu_valid,
            bins={"bin_idle": vsc.bin(0), "bin_requesting": vsc.bin(1)},
        )
        self.cp_cpu_addr = vsc.coverpoint(
            lambda: self._addr_trans,
            bins={"bin_sequential": vsc.bin(0), "bin_branch": vsc.bin(1),
                  "bin_same_line": vsc.bin(2), "bin_cross_line": vsc.bin(3)},
        )
        self.cp_cpu_la_valid = vsc.coverpoint(
            lambda: self._cpu_la_valid,
            bins={"bin_not_asserted": vsc.bin(0), "bin_asserted": vsc.bin(1)},
        )
        self.cp_cpu_la_addr = vsc.coverpoint(
            lambda: self._la_addr_rel,
            bins={"bin_matches_cpu_addr": vsc.bin(0), "bin_differs_index": vsc.bin(1), "bin_not_used": vsc.bin(2)},
        )
        self.cp_spi_ready = vsc.coverpoint(
            lambda: self._spi_ready,
            bins={"bin_not_ready": vsc.bin(0), "bin_ready": vsc.bin(1)},
        )
        self.cp_spi_rdata = vsc.coverpoint(
            lambda: self._spi_rdata_valid,
            bins={"bin_data_valid": vsc.bin(1)},
        )
        self.cp_cpu_ready = vsc.coverpoint(
            lambda: self._cpu_ready_type,
            bins={"bin_not_ready": vsc.bin(0), "bin_ready_0ws": vsc.bin(1),
                  "bin_ready_1ws": vsc.bin(2), "bin_ready_after_fill": vsc.bin(3)},
        )
        self.cp_cpu_rdata = vsc.coverpoint(
            lambda: self._cpu_rdata_valid,
            bins={"bin_data_delivered": vsc.bin(1)},
        )
        self.cp_spi_valid = vsc.coverpoint(
            lambda: self._spi_valid,
            bins={"bin_no_request": vsc.bin(0), "bin_requesting": vsc.bin(1)},
        )

    # Use vsc covergroup decorator equivalent manually
    def sample(self, dut, ref, mid_cycle):
        self._resetn = int(dut.resetn.value)
        self._cpu_valid = int(dut.cpu_valid.value)
        self._cpu_la_valid = int(dut.cpu_la_valid.value)
        self._spi_ready = int(dut.spi_ready.value)
        self._spi_valid = int(dut.spi_valid.value)
        self._cpu_rdata_valid = 1 if (int(dut.cpu_ready.value) and int(dut.cpu_valid.value)) else 0
        self._spi_rdata_valid = 1 if (int(dut.spi_valid.value) and int(dut.spi_ready.value)) else 0

        if int(dut.cpu_ready.value) == 0:
            self._cpu_ready_type = 0
        elif getattr(mid_cycle, "ready_cause", "") == "LA_HIT":
            self._cpu_ready_type = 1
        elif getattr(mid_cycle, "ready_cause", "") == "FALLBACK_HIT":
            self._cpu_ready_type = 2
        else:
            self._cpu_ready_type = 3

        if int(dut.cpu_la_valid.value) and int(dut.cpu_valid.value):
            if get_idx(int(dut.cpu_la_addr.value)) == get_idx(int(dut.cpu_addr.value)):
                self._la_addr_rel = 0
            else:
                self._la_addr_rel = 1
        elif int(dut.cpu_la_valid.value):
            self._la_addr_rel = 0
        else:
            self._la_addr_rel = 2

        prev = getattr(mid_cycle, "prev_addr", 0)
        curr = int(dut.cpu_addr.value)
        if int(dut.cpu_valid.value) == 0:
            self._addr_trans = 0
        elif prev == 0 or curr == prev + 4:
            self._addr_trans = 0
        elif get_idx(curr) == get_idx(prev) and get_tag(curr) == get_tag(prev):
            self._addr_trans = 2
        else:
            self._addr_trans = 1


# ===========================================================================
# 2.3 cg_hit_logic — Cache Hit Detection
# ===========================================================================
class cg_hit_logic(object):

    def __init__(self):
        tag_bins = {}
        for w in range(WAYS):
            tag_bins[f"bin_way_{w}"] = vsc.bin(w)
        tag_bins["bin_no_match"] = vsc.bin(WAYS)

        self.cp_hit_type = vsc.coverpoint(
            lambda: self._hit_type,
            bins={"bin_lookahead_hit": vsc.bin(0), "bin_fallback_hit": vsc.bin(1), "bin_miss": vsc.bin(2)},
        )
        self.cp_lookahead_used = vsc.coverpoint(
            lambda: self._lookahead_used,
            bins={"bin_yes": vsc.bin(1), "bin_no": vsc.bin(0)},
        )
        self.cp_tag_match = vsc.coverpoint(
            lambda: min(self._tag_match_way, WAYS),
            bins=tag_bins,
        )
        self.cp_valid_bit = vsc.coverpoint(
            lambda: min(self._valid_count, 2),
            bins={"bin_all_valid": vsc.bin(2), "bin_partial_valid": vsc.bin(1), "bin_all_invalid": vsc.bin(0)},
        )
        self.cp_wait_states = vsc.coverpoint(
            lambda: self._wait_states_type,
            bins={"bin_ws0": vsc.bin(0), "bin_ws1": vsc.bin(1), "bin_ws_fill": vsc.bin(2)},
        )

    def sample(self, dut, ref, mid_cycle):
        hm = getattr(mid_cycle, "hit_meta", {})
        self._hit_type = getattr(hm, "hit_type", 0)
        self._lookahead_used = 1 if getattr(hm, "lookahead_used", False) else 0
        self._tag_match_way = getattr(hm, "tag_match_way", WAYS)
        self._valid_count = getattr(hm, "valid_count", 0)
        self._wait_states_type = getattr(hm, "wait_states_type", 0)


# ===========================================================================
# 2.4 cg_miss_and_fill — Miss Handling & Line Fill
# ===========================================================================
class cg_miss_and_fill(object):

    def __init__(self):
        fc_bins = {}
        for i in range(WORDS_PER_LINE):
            fc_bins[f"bin_fc_{i}"] = vsc.bin(i)

        fb_bins = {}
        for i in range(WORDS_PER_LINE):
            fb_bins[f"bin_word_{i}"] = vsc.bin(i)

        vic_bins = {}
        for w in range(max(1, WAYS)):
            vic_bins[f"bin_way_0" if WAYS == 1 else f"bin_rr_way_{w}"] = vsc.bin(w)

        self.cp_miss_cause = vsc.coverpoint(
            lambda: self._miss_cause,
            bins={"bin_tag_mismatch": vsc.bin(0), "bin_invalid_entry": vsc.bin(1)},
        )
        self.cp_fill_cnt = vsc.coverpoint(
            lambda: min(self._fill_cnt, WORDS_PER_LINE - 1),
            bins=fc_bins,
        )
        self.cp_spi_lockstep = vsc.coverpoint(
            lambda: self._spi_lockstep_type,
            bins={"bin_advances_on_ready": vsc.bin(0), "bin_stalls": vsc.bin(1)},
        )
        self.cp_fill_buf_capture = vsc.coverpoint(
            lambda: min(self._fill_buf_word, WORDS_PER_LINE - 1),
            bins=fb_bins,
        )
        self.cp_victim_selection = vsc.coverpoint(
            lambda: min(self._victim_way, max(1, WAYS) - 1),
            bins=vic_bins,
        )
        self.cp_structural_commit = vsc.coverpoint(
            lambda: self._structural_commit_type,
            bins={"bin_read_old": vsc.bin(0), "bin_merge_new": vsc.bin(1), "bin_write_back": vsc.bin(2)},
        )
        self.cp_fill_complete = vsc.coverpoint(
            lambda: self._fill_complete_type,
            bins={"bin_cpu_ready_asserted": vsc.bin(1), "bin_returns_to_idle": vsc.bin(2), "bin_rr_advances": vsc.bin(3)},
        )

    def sample(self, dut, ref, mid_cycle):
        fm = getattr(mid_cycle, "fill_meta", {})
        self._miss_cause = getattr(fm, "miss_cause", 0)
        self._fill_cnt = getattr(fm, "fill_cnt", 0)
        self._spi_lockstep_type = getattr(fm, "spi_lockstep_type", 0)
        self._fill_buf_word = getattr(fm, "fill_buf_word", 0)
        self._victim_way = getattr(fm, "victim_way", 0)
        self._structural_commit_type = getattr(fm, "structural_commit_type", 0)
        self._fill_complete_type = getattr(fm, "fill_complete_type", 0) if getattr(fm, "fill_complete", False) else 0


# ===========================================================================
# 2.5 cg_init — Initialisation
# ===========================================================================
class cg_init(object):

    def __init__(self):
        init_idx_bins = {}
        for i in range(SETS):
            init_idx_bins[f"bin_i_{i}"] = vsc.bin(i)

        init_clr_bins = {}
        for w in range(WAYS):
            init_clr_bins[f"bin_way_{w}_cleared"] = vsc.bin(w)

        self.cp_init_trigger = vsc.coverpoint(
            lambda: self._init_trigger,
            bins={"bin_cold_boot": vsc.bin(0), "bin_reset_asserted": vsc.bin(1), "bin_reset_held": vsc.bin(2)},
        )
        self.cp_init_idx = vsc.coverpoint(
            lambda: min(self._init_idx, SETS - 1),
            bins=init_idx_bins,
        )
        self.cp_init_clear = vsc.coverpoint(
            lambda: min(self._init_clear_way, WAYS - 1),
            bins=init_clr_bins,
        )
        self.cp_init_complete = vsc.coverpoint(
            lambda: self._init_complete_type,
            bins={"bin_all_valid_cleared": vsc.bin(0), "bin_enters_idle": vsc.bin(1), "bin_cpu_held_off": vsc.bin(2)},
        )

    def sample(self, dut, ref, mid_cycle):
        im = getattr(mid_cycle, "init_meta", {})
        self._init_trigger = getattr(im, "init_trigger", 0)
        self._init_clear_way = getattr(im, "init_clear_way", 0)
        self._init_complete_type = getattr(im, "init_complete_type", 0)


# ===========================================================================
# 2.6 cg_replacement — Round-Robin Replacement
# ===========================================================================
class cg_replacement(object):

    def __init__(self):
        rr_bins = {}
        for w in range(max(1, WAYS)):
            rr_bins[f"bin_ctr_{w}"] = vsc.bin(w)

        self.cp_rr_pointer = vsc.coverpoint(
            lambda: min(self._rr_pointer, max(1, WAYS) - 1),
            bins=rr_bins,
        )
        self.cp_rr_advance = vsc.coverpoint(
            lambda: self._rr_advance_type,
            bins={"bin_advances_on_fill": vsc.bin(0), "bin_no_advance_direct_mapped": vsc.bin(1),
                  "bin_no_advance_no_fill": vsc.bin(2)},
        )
        self.cp_rr_wrap = vsc.coverpoint(
            lambda: self._rr_wrap,
            bins={"bin_wraps": vsc.bin(1), "bin_stays": vsc.bin(0)},
        )

    def sample(self, dut, ref, mid_cycle):
        rm = getattr(mid_cycle, "repl_meta", {})
        self._rr_pointer = ref._victim_ctr
        self._rr_advance_type = getattr(rm, "rr_advance_type", 0)
        self._rr_wrap = getattr(rm, "rr_wrap", 0)


# ===========================================================================
# 2.7 cg_memory_structure — Storage Array Integrity
# ===========================================================================
class cg_memory_structure(object):

    def __init__(self):
        self.cp_tag_mem_read = vsc.coverpoint(
            lambda: self._tag_mem_read,
            bins={"bin_read_hit_check": vsc.bin(0), "bin_read_pre_read": vsc.bin(1), "bin_read_fill": vsc.bin(2)},
        )
        self.cp_tag_mem_write = vsc.coverpoint(
            lambda: self._tag_mem_write,
            bins={"bin_write_init": vsc.bin(0), "bin_write_fill": vsc.bin(1)},
        )
        self.cp_data_mem_read = vsc.coverpoint(
            lambda: self._data_mem_read,
            bins={"bin_read_hit_check": vsc.bin(0), "bin_read_pre_read": vsc.bin(1), "bin_read_fill": vsc.bin(2)},
        )
        self.cp_data_mem_write = vsc.coverpoint(
            lambda: self._data_mem_write,
            bins={"bin_write_fill": vsc.bin(1)},
        )
        self.cp_atomic_rmw = vsc.coverpoint(
            lambda: self._atomic_rmw,
            bins={"bin_rmw_cycle": vsc.bin(1)},
        )

    def sample(self, dut, ref, mid_cycle):
        mm = getattr(mid_cycle, "mem_meta", {})
        self._tag_mem_read = getattr(mm, "tag_mem_read", 0)
        self._tag_mem_write = getattr(mm, "tag_mem_write", 0)
        self._data_mem_read = getattr(mm, "data_mem_read", 0)
        self._data_mem_write = getattr(mm, "data_mem_write", 0)
        self._atomic_rmw = 1 if getattr(mm, "atomic_rmw", False) else 0


# ===========================================================================
# 2.8 cg_address_decode — Address Field Partitioning
# ===========================================================================
class cg_address_decode(object):

    def __init__(self):
        off_bins = {}
        for i in range(max(1, WORDS_PER_LINE)):
            off_bins[f"bin_off_{i}"] = vsc.bin(i)

        idx_bins = {}
        for i in range(SETS):
            idx_bins[f"bin_idx_{i}"] = vsc.bin(i)

        self.cp_off_bits = vsc.coverpoint(
            lambda: min(self._off_val, max(1, WORDS_PER_LINE) - 1),
            bins=off_bins,
        )
        self.cp_idx_bits = vsc.coverpoint(
            lambda: min(self._idx_val, SETS - 1),
            bins=idx_bins,
        )
        self.cp_tag_bits = vsc.coverpoint(
            lambda: self._tag_val,
            bins={"bin_tag_value": vsc.bin([0, (1 << TAG_BITS) - 1]), "bin_tag_width": vsc.bin(TAG_BITS)},
        )
        self.cp_byte_offset = vsc.coverpoint(
            lambda: self._byte_offset,
            bins={"bin_ignored_00": vsc.bin(0)},
        )

    def sample(self, dut, ref, mid_cycle):
        addr = int(dut.cpu_addr.value) if int(dut.cpu_valid.value) else 0
        self._off_val = get_offset(addr)
        self._idx_val = get_idx(addr)
        self._tag_val = get_tag(addr)
        self._byte_offset = 0


# ===========================================================================
# 2.9 cg_fsm_states — FSM State & Transition Coverage
# ===========================================================================
class cg_fsm_states(object):

    def __init__(self):
        self.cp_fsm_current_state = vsc.coverpoint(
            lambda: self._current_state,
            bins={"bin_s_init": vsc.bin(0), "bin_s_idle": vsc.bin(1),
                  "bin_s_check": vsc.bin(2), "bin_s_fill": vsc.bin(3)},
        )
        self.cp_fsm_transition = vsc.coverpoint(
            lambda: self._transition,
            bins={
                "bin_init_to_idle": vsc.bin(0), "bin_idle_to_check": vsc.bin(1),
                "bin_idle_to_fill": vsc.bin(2), "bin_check_to_idle": vsc.bin(3),
                "bin_check_to_fill": vsc.bin(4), "bin_fill_to_idle": vsc.bin(5),
                "bin_fill_self": vsc.bin(6), "bin_fill_stall": vsc.bin(7),
                "bin_any_to_init": vsc.bin(8),
            },
        )
        self.cp_fsm_outputs = vsc.coverpoint(
            lambda: self._outputs_state,
            bins={
                "bin_init_outputs": vsc.bin(0), "bin_idle_outputs": vsc.bin(1),
                "bin_check_hit_out": vsc.bin(2), "bin_check_miss_out": vsc.bin(3),
                "bin_fill_out": vsc.bin(4), "bin_fill_done_out": vsc.bin(5),
            },
        )

    def sample(self, dut, ref, mid_cycle):
        sm = getattr(mid_cycle, "fsm_meta", {})
        if ref.in_init:
            self._current_state = 0
        elif ref.state == "IDLE":
            self._current_state = 1
        elif ref.state == "CHECK":
            self._current_state = 2
        else:
            self._current_state = 3
        self._transition = getattr(sm, "transition", 0)
        self._outputs_state = getattr(sm, "outputs_state", 0)


# ===========================================================================
# Cross Coverpoints
# ===========================================================================
class cg_crosses(object):

    def __init__(self):
        self.x_hit_vs_la = vsc.cross(
            [self.cp_hit_type, self.cp_lookahead_used],
        )
        self.x_wait_vs_hit = vsc.cross(
            [self.cp_wait_states, self.cp_hit_type],
        )
        self.x_addr_vs_hit = vsc.cross(
            [self.cp_idx_bits, self.cp_tag_bits, self.cp_hit_type],
        )
        self.x_victim_vs_rr = vsc.cross(
            [self.cp_victim_selection, self.cp_rr_pointer],
        )
        self.x_fill_vs_lockstep = vsc.cross(
            [self.cp_fill_cnt, self.cp_spi_lockstep],
        )
        self.x_fsm_vs_outputs = vsc.cross(
            [self.cp_fsm_current_state, self.cp_cpu_ready, self.cp_spi_valid],
        )
        self.x_miss_vs_ways = vsc.cross(
            [self.cp_miss_cause, self.cp_victim_selection],
        )

    def sample(self, dut, ref, mid_cycle):
        pass


# ===========================================================================
# Master collector — wraps all covergroups and handles the decorator DSL
# ===========================================================================
class CoverageCollector:
    """Collects functional coverage across all covergroups.

    Usage:
        cc = CoverageCollector()
        for each cycle:
            cc.sample(dut, ref, mid_cycle_state)
        cc.report()
    """

    def __init__(self):
        # Build covergroup instances using the functional DSL
        # The @vsc.covergroup decorator creates a class that wraps the instance

        @vsc.covergroup
        class cg_iface_cov(object):
            def __init__(self):
                self.options.weight = 1
                self.cp_resetn = vsc.coverpoint(lambda: _cg_i._resetn,
                    bins={"bin_deasserted": vsc.bin(1), "bin_asserted": vsc.bin(0)})
                self.cp_cpu_valid = vsc.coverpoint(lambda: _cg_i._cpu_valid,
                    bins={"bin_idle": vsc.bin(0), "bin_requesting": vsc.bin(1)})
                self.cp_cpu_addr = vsc.coverpoint(lambda: _cg_i._addr_trans,
                    bins={"bin_sequential": vsc.bin(0), "bin_branch": vsc.bin(1),
                          "bin_same_line": vsc.bin(2), "bin_cross_line": vsc.bin(3)})
                self.cp_cpu_la_valid = vsc.coverpoint(lambda: _cg_i._cpu_la_valid,
                    bins={"bin_not_asserted": vsc.bin(0), "bin_asserted": vsc.bin(1)})
                self.cp_cpu_la_addr = vsc.coverpoint(lambda: _cg_i._la_addr_rel,
                    bins={"bin_matches": vsc.bin(0), "bin_differs": vsc.bin(1), "bin_not_used": vsc.bin(2)})
                self.cp_spi_ready = vsc.coverpoint(lambda: _cg_i._spi_ready,
                    bins={"bin_not_ready": vsc.bin(0), "bin_ready": vsc.bin(1)})
                self.cp_spi_rdata = vsc.coverpoint(lambda: _cg_i._spi_rdata_valid,
                    bins={"bin_data_valid": vsc.bin(1)})
                self.cp_cpu_ready = vsc.coverpoint(lambda: _cg_i._cpu_ready_type,
                    bins={"bin_not_ready": vsc.bin(0), "bin_0ws": vsc.bin(1),
                          "bin_1ws": vsc.bin(2), "bin_fill": vsc.bin(3)})
                self.cp_cpu_rdata = vsc.coverpoint(lambda: _cg_i._cpu_rdata_valid,
                    bins={"bin_data_delivered": vsc.bin(1)})
                self.cp_spi_valid = vsc.coverpoint(lambda: _cg_i._spi_valid,
                    bins={"bin_no_req": vsc.bin(0), "bin_req": vsc.bin(1)})

        @vsc.covergroup
        class cg_hit_cov(object):
            def __init__(self):
                self.options.weight = 1
                tag_bins = {}
                for w in range(WAYS):
                    tag_bins[f"way_{w}"] = vsc.bin(w)
                tag_bins["no_match"] = vsc.bin(WAYS)
                self.cp_hit_type = vsc.coverpoint(lambda: _cg_h._hit_type,
                    bins={"la_hit": vsc.bin(0), "fb_hit": vsc.bin(1), "miss": vsc.bin(2)})
                self.cp_lookahead_used = vsc.coverpoint(lambda: _cg_h._lookahead_used,
                    bins={"yes": vsc.bin(1), "no": vsc.bin(0)})
                self.cp_tag_match = vsc.coverpoint(lambda: min(_cg_h._tag_match_way, WAYS), bins=tag_bins)
                self.cp_valid_bit = vsc.coverpoint(lambda: min(_cg_h._valid_count, 2),
                    bins={"all": vsc.bin(2), "partial": vsc.bin(1), "none": vsc.bin(0)})
                self.cp_wait_states = vsc.coverpoint(lambda: _cg_h._ws,
                    bins={"ws0": vsc.bin(0), "ws1": vsc.bin(1), "fill": vsc.bin(2)})

        @vsc.covergroup
        class cg_fill_cov(object):
            def __init__(self):
                self.options.weight = 1
                fc_bins = {}
                for i in range(WORDS_PER_LINE):
                    fc_bins[f"fc_{i}"] = vsc.bin(i)
                fb_bins = {}
                for i in range(WORDS_PER_LINE):
                    fb_bins[f"w_{i}"] = vsc.bin(i)
                vic_bins = {}
                mw = max(1, WAYS)
                for w in range(mw):
                    pfx = "w0" if WAYS == 1 else f"rr_{w}"
                    vic_bins[pfx] = vsc.bin(w)
                self.cp_miss_cause = vsc.coverpoint(lambda: _cg_f._miss_cause,
                    bins={"tag": vsc.bin(0), "inv": vsc.bin(1)})
                self.cp_fill_cnt = vsc.coverpoint(lambda: min(_cg_f._fill_cnt, WORDS_PER_LINE-1), bins=fc_bins)
                self.cp_spi_lockstep = vsc.coverpoint(lambda: _cg_f._spi_ls,
                    bins={"adv": vsc.bin(0), "stall": vsc.bin(1)})
                self.cp_fill_buf = vsc.coverpoint(lambda: min(_cg_f._fb_word, WORDS_PER_LINE-1), bins=fb_bins)
                self.cp_victim = vsc.coverpoint(lambda: min(_cg_f._vic, max(1, WAYS)-1), bins=vic_bins)
                self.cp_commit = vsc.coverpoint(lambda: _cg_f._commit,
                    bins={"read": vsc.bin(0), "merge": vsc.bin(1), "wb": vsc.bin(2)})
                self.cp_done = vsc.coverpoint(lambda: _cg_f._done_type,
                    bins={"rdy": vsc.bin(1), "idle": vsc.bin(2), "rr": vsc.bin(3)})

        @vsc.covergroup
        class cg_init_cov(object):
            def __init__(self):
                self.options.weight = 1
                iib = {}
                for i in range(SETS):
                    iib[f"i_{i}"] = vsc.bin(i)
                icb = {}
                for w in range(WAYS):
                    icb[f"clr_{w}"] = vsc.bin(w)
                self.cp_init_trig = vsc.coverpoint(lambda: _cg_ni._trig,
                    bins={"cold": vsc.bin(0), "rst": vsc.bin(1), "held": vsc.bin(2)})
                self.cp_init_idx = vsc.coverpoint(lambda: min(_cg_ni._idx, SETS-1), bins=iib)
                self.cp_init_clr = vsc.coverpoint(lambda: min(_cg_ni._clr, WAYS-1), bins=icb)
                self.cp_init_done = vsc.coverpoint(lambda: _cg_ni._done,
                    bins={"clr": vsc.bin(0), "enter": vsc.bin(1), "held_off": vsc.bin(2)})

        @vsc.covergroup
        class cg_repl_cov(object):
            def __init__(self):
                self.options.weight = 1
                rb = {}
                for w in range(max(1, WAYS)):
                    rb[f"ctr_{w}"] = vsc.bin(w)
                self.cp_ptr = vsc.coverpoint(lambda: min(_cg_r._ptr, max(1, WAYS)-1), bins=rb)
                self.cp_adv = vsc.coverpoint(lambda: _cg_r._adv,
                    bins={"on_fill": vsc.bin(0), "dm": vsc.bin(1), "no_fill": vsc.bin(2)})
                self.cp_wrap = vsc.coverpoint(lambda: _cg_r._wrap,
                    bins={"wraps": vsc.bin(1), "stays": vsc.bin(0)})

        @vsc.covergroup
        class cg_mem_cov(object):
            def __init__(self):
                self.options.weight = 1
                self.cp_tag_r = vsc.coverpoint(lambda: _cg_m._tag_r,
                    bins={"hit": vsc.bin(0), "pre": vsc.bin(1), "fill": vsc.bin(2)})
                self.cp_tag_w = vsc.coverpoint(lambda: _cg_m._tag_w,
                    bins={"init": vsc.bin(0), "fill": vsc.bin(1)})
                self.cp_dat_r = vsc.coverpoint(lambda: _cg_m._dat_r,
                    bins={"hit": vsc.bin(0), "pre": vsc.bin(1), "fill": vsc.bin(2)})
                self.cp_dat_w = vsc.coverpoint(lambda: _cg_m._dat_w,
                    bins={"fill": vsc.bin(1)})
                self.cp_rmw = vsc.coverpoint(lambda: _cg_m._rmw,
                    bins={"cycle": vsc.bin(1)})

        @vsc.covergroup
        class cg_addr_cov(object):
            def __init__(self):
                self.options.weight = 1
                ob = {}
                for i in range(max(1, WORDS_PER_LINE)):
                    ob[f"off_{i}"] = vsc.bin(i)
                xb = {}
                for i in range(SETS):
                    xb[f"idx_{i}"] = vsc.bin(i)
                self.cp_off = vsc.coverpoint(lambda: min(_cg_a._off, max(1, WORDS_PER_LINE)-1), bins=ob)
                self.cp_idx = vsc.coverpoint(lambda: min(_cg_a._idx, SETS-1), bins=xb)
                self.cp_tag = vsc.coverpoint(lambda: _cg_a._tag,
                    bins={"val": vsc.bin([0, (1 << TAG_BITS)-1]), "width": vsc.bin(TAG_BITS)})
                self.cp_byte = vsc.coverpoint(lambda: _cg_a._byte,
                    bins={"ignored_00": vsc.bin(0)})

        @vsc.covergroup
        class cg_fsm_cov(object):
            def __init__(self):
                self.options.weight = 1
                self.cp_state = vsc.coverpoint(lambda: _cg_s._state,
                    bins={"init": vsc.bin(0), "idle": vsc.bin(1), "check": vsc.bin(2), "fill": vsc.bin(3)})
                self.cp_trans = vsc.coverpoint(lambda: _cg_s._trans,
                    bins={"i2i": vsc.bin(0), "i2c": vsc.bin(1), "i2f": vsc.bin(2), "c2i": vsc.bin(3),
                          "c2f": vsc.bin(4), "f2i": vsc.bin(5), "f_self": vsc.bin(6), "f_stall": vsc.bin(7),
                          "any2i": vsc.bin(8)})
                self.cp_out = vsc.coverpoint(lambda: _cg_s._out,
                    bins={"i": vsc.bin(0), "id": vsc.bin(1), "ch": vsc.bin(2), "cm": vsc.bin(3),
                          "f": vsc.bin(4), "fd": vsc.bin(5)})

        self.cg_iface = cg_iface_cov()
        self.cg_hit = cg_hit_cov()
        self.cg_fill = cg_fill_cov()
        self.cg_init = cg_init_cov()
        self.cg_repl = cg_repl_cov()
        self.cg_mem = cg_mem_cov()
        self.cg_addr = cg_addr_cov()
        self.cg_fsm = cg_fsm_cov()

    def sample(self, dut, ref, mid_cycle):
        _update_cg_i(dut, ref, mid_cycle)
        _update_cg_h(dut, ref, mid_cycle)
        _update_cg_f(dut, ref, mid_cycle)
        _update_cg_ni(dut, ref, mid_cycle)
        _update_cg_r(dut, ref, mid_cycle)
        _update_cg_m(dut, ref, mid_cycle)
        _update_cg_a(dut, ref, mid_cycle)
        _update_cg_s(dut, ref, mid_cycle)

        self.cg_iface.sample()
        self.cg_hit.sample()
        self.cg_fill.sample()
        self.cg_init.sample()
        self.cg_repl.sample()
        self.cg_mem.sample()
        self.cg_addr.sample()
        self.cg_fsm.sample()

    def report(self, details=False):
        try:
            with open(os.devnull, "w") as devnull, contextlib.redirect_stdout(devnull):
                report = vsc.get_coverage_report_model()
            total = report.coverage
            print(f"\n=== Coverage Report ===\nTotal: {total:.1f}%\n")
            if details:
                for cg in report.covergroups:
                    print(f"  {cg.name}: {cg.coverage:.1f}%")
                    for cp in cg.coverpoints:
                        hit = sum(1 for b in cp.bins if b.hit)
                        print(f"    {cp.name}: {cp.coverage:.1f}%  ({hit}/{len(cp.bins)} bins hit)")
            return total
        except Exception:
            print("Coverage report error:", file=sys.stderr)
            traceback.print_exc(file=sys.stderr)
            return 0.0

    def save(self, path="coverage.xml", fmt=None):
        if fmt is None:
            if path.endswith(".json"):
                fmt = "json"
            else:
                fmt = "xml"
        try:
            if fmt == "xml":
                with open(os.devnull, "w") as devnull, contextlib.redirect_stdout(devnull):
                    vsc.write_coverage_db(path, fmt="xml")
            elif fmt == "json":
                with open(os.devnull, "w") as devnull, contextlib.redirect_stdout(devnull):
                    report = vsc.get_coverage_report_model()
                data = self._report_to_dict(report)
                with open(path, "w") as f:
                    json.dump(data, f, indent=2)
            else:
                raise ValueError(f"Unsupported format: {fmt}")
        except Exception:
            print(f"Coverage save error ({path}):", file=sys.stderr)
            traceback.print_exc(file=sys.stderr)

    @staticmethod
    def _report_to_dict(report):
        def cg_to_dict(cg):
            return {
                "name": cg.name,
                "instname": cg.instname,
                "coverage": cg.coverage,
                "weight": cg.weight,
                "coverpoints": [_cp_to_dict(cp) for cp in cg.coverpoints],
                "crosses": [_cross_to_dict(cr) for cr in cg.crosses],
            }

        def _cp_to_dict(cp):
            return {
                "name": cp.name,
                "coverage": cp.coverage,
                "weight": cp.weight,
                "bins_total": len(cp.bins),
                "bins_hit": sum(1 for b in cp.bins if b.hit),
                "bins": [{"name": b.name, "count": b.count, "goal": b.goal, "hit": b.hit} for b in cp.bins],
            }

        def _cross_to_dict(cr):
            return {
                "name": cr.name,
                "coverage": cr.coverage,
                "weight": cr.weight,
                "bins_total": len(cr.bins),
                "bins_hit": sum(1 for b in cr.bins if b.hit),
            }

        return {
            "total_coverage": report.coverage,
            "covergroups": [cg_to_dict(cg) for cg in report.covergroups],
        }


# ===========================================================================
# Module-level state objects (set by sample() before covergroup sampling)
# ===========================================================================
class _CGState:
    pass


_cg_i = _CGState()   # interface
_cg_h = _CGState()   # hit logic
_cg_f = _CGState()   # miss & fill
_cg_ni = _CGState()  # init
_cg_r = _CGState()   # replacement
_cg_m = _CGState()   # memory
_cg_a = _CGState()   # address decode
_cg_s = _CGState()   # FSM states


# ===========================================================================
# Module-level collector and convenience API
# ===========================================================================
_collector = None
_flush_hooks = []


def register_flush_hook(fn):
    """Register a callable to flush pending coverage samples before reporting."""
    _flush_hooks.append(fn)


def _flush_all():
    for hook in _flush_hooks:
        try:
            hook()
        except Exception:
            pass


def _get_collector():
    global _collector
    if _collector is None:
        _collector = CoverageCollector()
    return _collector


def sample_coverage(dut, ref, mid_cycle=None):
    """Sample coverage for one clock cycle. Call after each ref.step() / RisingEdge.

    mid_cycle may be omitted; if not provided, the collector reads from
    ref._mid_cycle (set by CacheRef.step()).
    """
    if not _COVERAGE_ENABLED:
        return
    if mid_cycle is None:
        mid_cycle = getattr(ref, "_mid_cycle", None)
        if mid_cycle is None:
            return
    try:
        _get_collector().sample(dut, ref, mid_cycle)
    except Exception:
        pass  # suppressed in hot loop; errors go to nowhere so we stay quiet


def report_coverage(details=False):
    """Print a coverage report. Returns the total coverage percentage."""
    if not _COVERAGE_ENABLED:
        return 0.0
    return _get_collector().report(details=details)


def save_coverage(path, fmt=None):
    """Save coverage data to a file (xml or json format auto-detected from extension)."""
    if not _COVERAGE_ENABLED:
        return
    _get_collector().save(path, fmt=fmt)


def _finalize_coverage():
    """Called at exit: flushes, reports to stdout, saves to coverage.xml/.json and ts copies."""
    if not _COVERAGE_ENABLED:
        return
    try:
        _flush_all()
        try:
            report_coverage(details=True)
        except Exception:
            print("Coverage report error:", file=sys.stderr)
            traceback.print_exc(file=sys.stderr)
        _save_both(_COVERAGE_XML, _COVERAGE_JSON)
        ts = _ts_tag()
        _save_both(f"coverage_{ts}.xml", f"coverage_{ts}.json")
    except Exception:
        print("Coverage finalize error:", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)


def _save_both(xml_name, json_name):
    xml_path = _cov_path(xml_name)
    json_path = _cov_path(json_name)
    save_coverage(xml_path)
    save_coverage(json_path)
    written = []
    if os.path.exists(xml_path):
        written.append(xml_path)
    if os.path.exists(json_path):
        written.append(json_path)
    if written:
        msg = f"Coverage files written: {', '.join(written)}"
        print(msg)
        sys.stdout.flush()


atexit.register(_finalize_coverage)


class _Finalizer:
    """Last-resort backup: if atexit doesn't fire (Verilator), __del__ writes ts files."""
    def __del__(self):
        if not _COVERAGE_ENABLED:
            return
        try:
            _flush_all()
        except Exception:
            pass
        try:
            ts = _ts_tag()
            _save_both(f"coverage_{ts}.xml", f"coverage_{ts}.json")
        except Exception:
            pass


_finalizer = _Finalizer()


def _update_cg_i(dut, ref, mid_cycle):
    """Update interface covergroup state."""
    _cg_i._resetn = int(dut.resetn.value)
    _cg_i._cpu_valid = int(dut.cpu_valid.value)
    _cg_i._cpu_la_valid = int(dut.cpu_la_valid.value)
    _cg_i._spi_ready = int(dut.spi_ready.value)
    _cg_i._spi_valid = int(dut.spi_valid.value)
    _cg_i._cpu_rdata_valid = 1 if ref.cpu_ready else 0
    _cg_i._spi_rdata_valid = 1 if (int(dut.spi_valid.value) and int(dut.spi_ready.value)) else 0

    if ref.cpu_ready == 0:
        _cg_i._cpu_ready_type = 0
    elif getattr(mid_cycle, "ready_cause", "") == "LA_HIT":
        _cg_i._cpu_ready_type = 1
    elif getattr(mid_cycle, "ready_cause", "") == "FALLBACK_HIT":
        _cg_i._cpu_ready_type = 2
    else:
        _cg_i._cpu_ready_type = 3

    if int(dut.cpu_la_valid.value) and int(dut.cpu_valid.value):
        if get_idx(int(dut.cpu_la_addr.value)) == get_idx(int(dut.cpu_addr.value)):
            _cg_i._la_addr_rel = 0
        else:
            _cg_i._la_addr_rel = 1
    elif int(dut.cpu_la_valid.value):
        _cg_i._la_addr_rel = 0
    else:
        _cg_i._la_addr_rel = 2

    prev = getattr(mid_cycle, "prev_addr", 0)
    curr = int(dut.cpu_addr.value)
    if int(dut.cpu_valid.value) == 0:
        _cg_i._addr_trans = 0
    elif prev == 0 or curr == prev + 4:
        _cg_i._addr_trans = 0
    elif get_idx(curr) == get_idx(prev) and get_tag(curr) == get_tag(prev):
        _cg_i._addr_trans = 2
    elif get_idx(curr) == get_idx(prev) and get_tag(curr) != get_tag(prev):
        _cg_i._addr_trans = 3
    else:
        _cg_i._addr_trans = 1


def _update_cg_h(dut, ref, mid_cycle):
    hm = getattr(mid_cycle, "hit_meta", {})
    _cg_h._hit_type = getattr(hm, "hit_type", 0)
    _cg_h._lookahead_used = 1 if getattr(hm, "lookahead_used", False) else 0
    _cg_h._tag_match_way = getattr(hm, "tag_match_way", WAYS)
    _cg_h._valid_count = getattr(hm, "valid_count", 0)
    _cg_h._ws = getattr(hm, "wait_states_type", 0)


def _update_cg_f(dut, ref, mid_cycle):
    fm = getattr(mid_cycle, "fill_meta", {})
    _cg_f._miss_cause = getattr(fm, "miss_cause", 0)
    _cg_f._fill_cnt = getattr(fm, "fill_cnt", 0)
    _cg_f._spi_ls = getattr(fm, "spi_lockstep_type", 0)
    _cg_f._fb_word = getattr(fm, "fill_buf_word", 0)
    _cg_f._vic = getattr(fm, "victim_way", 0)
    _cg_f._commit = getattr(fm, "structural_commit_type", 0)
    _cg_f._done_type = getattr(fm, "fill_complete_type", 0) if getattr(fm, "fill_complete", False) else 0


def _update_cg_ni(dut, ref, mid_cycle):
    im = getattr(mid_cycle, "init_meta", {})
    _cg_ni._trig = getattr(im, "init_trigger", 0)
    _cg_ni._idx = ref._init_idx if ref.in_init else SETS
    _cg_ni._clr = getattr(im, "init_clear_way", 0)
    _cg_ni._done = getattr(im, "init_complete_type", 0)


def _update_cg_r(dut, ref, mid_cycle):
    rm = getattr(mid_cycle, "repl_meta", {})
    _cg_r._ptr = ref._victim_ctr
    _cg_r._adv = getattr(rm, "rr_advance_type", 0)
    _cg_r._wrap = getattr(rm, "rr_wrap", 0)


def _update_cg_m(dut, ref, mid_cycle):
    mm = getattr(mid_cycle, "mem_meta", {})
    _cg_m._tag_r = getattr(mm, "tag_mem_read", 0)
    _cg_m._tag_w = getattr(mm, "tag_mem_write", 0)
    _cg_m._dat_r = getattr(mm, "data_mem_read", 0)
    _cg_m._dat_w = getattr(mm, "data_mem_write", 0)
    _cg_m._rmw = 1 if getattr(mm, "atomic_rmw", False) else 0


def _update_cg_a(dut, ref, mid_cycle):
    addr = int(dut.cpu_addr.value) if int(dut.cpu_valid.value) else 0
    _cg_a._off = get_offset(addr)
    _cg_a._idx = get_idx(addr)
    _cg_a._tag = get_tag(addr)
    _cg_a._byte = 0


def _update_cg_s(dut, ref, mid_cycle):
    sm = getattr(mid_cycle, "fsm_meta", {})
    if ref.in_init:
        _cg_s._state = 0
    elif ref.state == "IDLE":
        _cg_s._state = 1
    elif ref.state == "CHECK":
        _cg_s._state = 2
    else:
        _cg_s._state = 3
    _cg_s._trans = getattr(sm, "transition", 0)
    _cg_s._out = getattr(sm, "outputs_state", 0)
