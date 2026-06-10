# test_hit.py

import cocotb
from cocotb.triggers import RisingEdge, ClockCycles

from conftest import (
    clock_gen, reset, create_rng,
    SETS, WAYS, WORDS_PER_LINE, ITERATIONS,
    TAG_BITS, get_idx, get_tag, get_offset, make_addr,
    CoverageTrackedRef,
)
from stim import random_addr


@cocotb.test()
async def test_hit(dut):
    """Verify hit data integrity."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    # Phase 1: Populate the cache
    populated = {}
    num_sets = min(SETS, 16)
    target_sets = rng.sample(range(SETS), num_sets)

    for set_idx in target_sets:
        populated[set_idx] = {}
        for way_idx in range(WAYS):
            tag = rng.randint(0, (1 << TAG_BITS) - 1)
            existing = [t for (t, _) in populated[set_idx].values()]
            while tag in existing:
                tag = rng.randint(0, (1 << TAG_BITS) - 1)
            data_words = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
            populated[set_idx][way_idx] = (tag, data_words)
            await _fill_line(dut, ref, set_idx, tag, data_words)

    # Phase 2: Hit verification — just check data, timing is deterministic
    for _ in range(ITERATIONS):
        set_idx = rng.choice(target_sets)
        way_idx = rng.randint(0, WAYS - 1)
        tag, data_words = populated[set_idx][way_idx]
        off = rng.randint(0, max(1, WORDS_PER_LINE) - 1)
        addr = make_addr(tag, set_idx, off)

        # Fast path: look-ahead + request, 1-cycle hit
        la_addr = random_addr(rng, constrain_idx=set_idx)
        dut.cpu_la_valid.value = 1
        dut.cpu_la_addr.value = la_addr
        dut.cpu_valid.value = 0
        ref.step(1, la_addr, 0, 0, 0, 0)
        await RisingEdge(dut.clk)

        dut.cpu_la_valid.value = 0
        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr
        ref.step(0, 0, 1, addr, 0, 0)
        await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data_words[off], "fast path data mismatch"
    await _cleanup(dut, ref)

    # Fallback: no look-ahead, 2-cycle hit
    dut.cpu_la_valid.value = 0
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr
    ref.step(0, 0, 1, addr, 0, 0)
    await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data_words[off], "fallback data mismatch"
    await _cleanup(dut, ref)

    # Multi-word line offset checks
    if WORDS_PER_LINE > 1:
        for _ in range(min(ITERATIONS, 100)):
            set_idx = rng.choice(target_sets)
            way_idx = rng.randint(0, WAYS - 1)
            tag, data_words = populated[set_idx][way_idx]
            for off in range(WORDS_PER_LINE):
                addr = make_addr(tag, set_idx, off)
                expected = data_words[off]
                dut.cpu_valid.value = 1
                dut.cpu_addr.value = addr
                dut.cpu_la_valid.value = 0
                ref.step(0, 0, 1, addr, 0, 0)
                await RisingEdge(dut.clk)
                await _wait(dut, ref, 2)
                assert dut.cpu_rdata.value == expected, f"offset {off} data mismatch"
                await _cleanup(dut, ref)

    # --- Fast miss (idle_to_fill / i2f transition): look-ahead + miss ---
    # Look-ahead pre-reads a set, same-index request with an untagged tag
    # causes the FSM to go directly s_idle -> s_fill (transition 2).
    for _ in range(min(ITERATIONS, 50)):
        set_idx = rng.choice(target_sets)
        unused_tag = rng.randint(0, (1 << TAG_BITS) - 1)
        existing = [t for (t, _) in populated[set_idx].values()]
        while unused_tag in existing:
            unused_tag = rng.randint(0, (1 << TAG_BITS) - 1)
        addr = make_addr(unused_tag, set_idx, 0)
        data_words = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]

        # Look-ahead with same index
        la_addr = random_addr(rng, constrain_idx=set_idx)
        dut.cpu_la_valid.value = 1
        dut.cpu_la_addr.value = la_addr
        dut.cpu_valid.value = 0
        ref.step(1, la_addr, 0, 0, 0, 0)
        await RisingEdge(dut.clk)

        # Request with same index but untagged tag -> fast miss
        dut.cpu_la_valid.value = 0
        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr
        ref.step(0, 0, 1, addr, 0, 0)
        await RisingEdge(dut.clk)

        # Now DUT should be in FILL; feed the SPI words
        dut.cpu_valid.value = 0
        dut.spi_ready.value = 0
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)

        for wi in range(WORDS_PER_LINE):
            dut.spi_ready.value = 1
            dut.spi_rdata.value = data_words[wi]
            dut.cpu_valid.value = 0
            ref.step(0, 0, 0, 0, 1, data_words[wi])
            await RisingEdge(dut.clk)

        await _wait(dut, ref, 2)
        assert dut.cpu_rdata.value == data_words[0], "fast miss data mismatch"
        assert ref.state == "IDLE", f"ref should be IDLE after fast miss fill, got {ref.state}"
        await _cleanup(dut, ref)

    ref.save_snapshot()


async def _fill_line(dut, ref, set_idx, tag, data_words):
    addr = make_addr(tag, set_idx, 0)
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr, 0, 0)
    await RisingEdge(dut.clk)

    dut.cpu_valid.value = 0
    dut.spi_ready.value = 0
    ref.step(0, 0, 0, 0, 0, 0)
    await RisingEdge(dut.clk)

    for wi in range(WORDS_PER_LINE):
        dut.spi_ready.value = 1
        dut.spi_rdata.value = data_words[wi]
        dut.cpu_valid.value = 0
        ref.step(0, 0, 0, 0, 1, data_words[wi])
        await RisingEdge(dut.clk)

    await _wait(dut, ref, 2)
    assert ref.state == "IDLE"


async def _wait(dut, ref, n):
    """Advance n idle cycles."""
    for _ in range(n):
        dut.cpu_valid.value = 0
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)


async def _cleanup(dut, ref):
    """One idle cycle."""
    dut.cpu_valid.value = 0
    ref.step(0, 0, 0, 0, 0, 0)
    await RisingEdge(dut.clk)
unit='ns'