# conftest.py — cocotb configuration, env parsing, clock/reset fixtures, address helpers

import os
import random

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, ClockCycles, Timer

# ---- environment variables ----
SETS           = int(os.environ.get("SETS", "64"))
WAYS           = int(os.environ.get("WAYS", "4"))
WORDS_PER_LINE = int(os.environ.get("WORDS_PER_LINE", "4"))
ITERATIONS     = int(os.environ.get("ITERATIONS", "1000"))
_seed_raw = os.environ.get("SEED", "")
SEED       = int(_seed_raw) if _seed_raw else random.randint(0, 0xFFFFFFFF)

# ---- derived constants ----
OFF_BITS = (WORDS_PER_LINE.bit_length() - 1) if WORDS_PER_LINE > 1 else 0
assert WORDS_PER_LINE & (WORDS_PER_LINE - 1) == 0, "WORDS_PER_LINE must be power of 2"
assert SETS & (SETS - 1) == 0, "SETS must be power of 2"
IDX_BITS = SETS.bit_length() - 1
TAG_BITS = 24 - 2 - OFF_BITS - IDX_BITS
assert TAG_BITS > 0, f"TAG_BITS must be positive, got {TAG_BITS}"
TAGW  = TAG_BITS + 1
LINE  = WORDS_PER_LINE * 32
DATAW = WAYS * LINE
TAGMW = WAYS * TAGW
FCW   = OFF_BITS if OFF_BITS > 0 else 1
VW    = max(1, WAYS.bit_length() - 1)


# ---- address field extraction ----
def get_idx(addr):
    """Extract index bits from 24-bit byte address."""
    addr = int(addr)
    return (addr >> (2 + OFF_BITS)) & ((1 << IDX_BITS) - 1)


def get_tag(addr):
    """Extract tag bits from 24-bit byte address."""
    addr = int(addr)
    return (addr >> (2 + OFF_BITS + IDX_BITS)) & ((1 << TAG_BITS) - 1)


def get_offset(addr):
    """Extract word offset within cache line."""
    addr = int(addr)
    if OFF_BITS == 0:
        return 0
    return (addr >> 2) & ((1 << OFF_BITS) - 1)


def line_base_from_addr(addr):
    """Return the byte address of the first word of the line containing addr."""
    addr = int(addr)
    return addr & ~((1 << (2 + OFF_BITS)) - 1)


def make_addr(tag, idx, off=0, byte_off=0):
    """Compose a 24-bit byte address from tag, index, offset, and low-2 byte-offset."""
    addr = byte_off & 0x3
    addr |= (off & ((1 << max(OFF_BITS, 1)) - 1)) << 2
    addr |= (idx & ((1 << IDX_BITS) - 1)) << (2 + OFF_BITS)
    addr |= (tag & ((1 << TAG_BITS) - 1)) << (2 + OFF_BITS + IDX_BITS)
    return addr & 0xFFFFFF


# ---- cocotb fixtures ----
async def clock_gen(dut, period_ns=10):
    """Start a free-running clock on dut.clk."""
    clock = Clock(dut.clk, period_ns, unit="ns")
    cocotb.start_soon(clock.start())


async def reset(dut, cycles=5):
    """Assert resetn for cycles, then release."""
    dut.resetn.value = 0
    dut.cpu_valid.value = 0
    dut.cpu_la_valid.value = 0
    dut.spi_ready.value = 0
    dut.spi_rdata.value = 0
    dut.cpu_addr.value = 0
    dut.cpu_la_addr.value = 0
    await ClockCycles(dut.clk, cycles)
    dut.resetn.value = 1
    await RisingEdge(dut.clk)


# ---- cocotb test setup ----
def create_rng(seed=None):
    """Create a random.Random instance from SEED."""
    if seed is None:
        seed = SEED
    return random.Random(seed)


async def advance_idle(dut, ref):
    """Advance one idle cycle so DUT output registers settle.
    Call before reading dut.cpu_ready / dut.spi_valid / dut.cpu_rdata
    to work around a Verilator/cocotb timing quirk where non-blocking
    assignments are not visible until one full clock cycle after the
    RisingEdge that schedules them.
    """
    dut.cpu_valid.value = 0
    dut.cpu_la_valid.value = 0
    dut.spi_ready.value = 0
    ref.step(0, 0, 0, 0, 0, 0)
    await RisingEdge(dut.clk)


async def settle(dut):
    """Wait one delta cycle for Verilator NBAs to commit.  Prefer this over
    advance_idle when you only need read visibility without consuming a full
    clock cycle."""
    await Timer(1, units='step')


# ---- coverage helpers ----
class CoverageRunner:
    """Optional context manager for per-test coverage tracking.

    Usage::

        async with CoverageRunner(dut) as cov:
            for cycle in range(N):
                cov.step(ref)
                await RisingEdge(dut.clk)
            # prints report + saves xml/json on exit
    """
    def __init__(self, dut, *, report=True, xml="coverage.xml", json="coverage.json"):
        self.dut = dut
        self.report = report
        self.xml = xml
        self.json = json
        self.ref = None

    async def __aenter__(self):
        from coverage import sample_coverage
        self._sample = sample_coverage
        return self

    async def __aexit__(self, *args):
        from coverage import report_coverage, save_coverage
        if self.report:
            report_coverage(details=True)
        if self.xml:
            save_coverage(self.xml)
        if self.json:
            save_coverage(self.json)

    def step(self, ref):
        """Call after ref.step() but before RisingEdge to sample coverage for this cycle."""
        if ref is not None:
            self.ref = ref
        self._sample(self.dut, ref)


class CoverageTrackedRef:
    """Wrapper around CacheRef that auto-samples coverage on step().

    Sampling is deferred by one cycle so that DUT outputs settle after
    RisingEdge before they are read.  The final pending cycle is flushed
    by the atexit handler in coverage.py before the consolidated report.

    Usage::

        from conftest import CoverageTrackedRef
        ref = CoverageTrackedRef(dut)
        ref.step(0, 0, 1, addr, 0, 0)  # coverage auto-sampled (deferred)
    """
    def __init__(self, dut):
        self.dut = dut
        from cache_ref import CacheRef
        self.ref = CacheRef()
        from coverage import sample_coverage, report_coverage, save_coverage, register_flush_hook
        self._sample = sample_coverage
        self._report = report_coverage
        self._save = save_coverage
        self._pending_mc = None
        self._cycles = 0
        register_flush_hook(self.flush)

    def __getattr__(self, name):
        return getattr(self.ref, name)

    def step(self, *args, **kwargs):
        # Sample PREVIOUS cycle now that DUT outputs have settled via RisingEdge
        if self._pending_mc is not None:
            self._sample(self.dut, self.ref, mid_cycle=self._pending_mc)
            self._cycles += 1
        result = self.ref.step(*args, **kwargs)
        self._pending_mc = result[4]
        return result

    def flush(self):
        """Flush the final pending coverage sample (called before report)."""
        if self._pending_mc is not None:
            self._sample(self.dut, self.ref, mid_cycle=self._pending_mc)
            self._cycles += 1
            self._pending_mc = None

    def report(self, details=False):
        return self._report(details=details)

    def save(self, path="coverage.xml", fmt=None):
        self.flush()
        self._save(path, fmt=fmt)

    def save_snapshot(self):
        """Flush and write both coverage.xml/.json and timestamped copies for consolidation."""
        self.flush()
        import time
        from coverage import _save_both
        _save_both("coverage.xml", "coverage.json")
        ts = time.strftime("%Y%m%d_%H%M%S") + f"_{int(time.time() * 1e6) % 1000000:06d}"
        _save_both(f"coverage_{ts}.xml", f"coverage_{ts}.json")

    def __del__(self):
        try:
            self.flush()
        except Exception:
            pass
        try:
            from coverage import _save_both, _COVERAGE_ENABLED
            import time
            if _COVERAGE_ENABLED:
                ts = time.strftime("%Y%m%d_%H%M%S")
                _save_both(f"coverage_{ts}.xml", f"coverage_{ts}.json")
        except Exception:
            pass

    @property
    def mid_cycle(self):
        return self.ref._mid_cycle
