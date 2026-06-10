# test_miss.py — Miss detection and line fill verification

import cocotb
from cocotb.triggers import RisingEdge

from conftest import (
    clock_gen, reset, create_rng,
    SETS, WAYS, WORDS_PER_LINE, ITERATIONS,
    TAG_BITS, make_addr,
    CoverageTrackedRef,
)
from stim import random_spi_gating


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


@cocotb.test()
async def test_miss(dut):
    """Verify miss detection, streaming fill, SPI lockstep, and post-fill hit."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    for iteration in range(ITERATIONS):
        p_stall = rng.uniform(0.0, 0.5)
        gating = random_spi_gating(rng, p_stall)

        set_idx = rng.randint(0, SETS - 1)
        off = rng.randint(0, max(1, WORDS_PER_LINE) - 1)
        tag = rng.randint(0, (1 << TAG_BITS) - 1)
        addr = make_addr(tag, set_idx, off)

        data_words = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]

        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr
        dut.cpu_la_valid.value = 0
        ref.step(0, 0, 1, addr, 0, 0)
        await RisingEdge(dut.clk)

        dut.cpu_valid.value = 0
        dut.spi_ready.value = 0
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)

        await _wait(dut, ref, 3)
        assert dut.spi_valid.value == 1, f"spi_valid should be 1 after miss"

        word_idx = 0
        fill_cycles = 0
        while True:
            ready = next(gating)
            assert dut.spi_valid.value == 1, f"spi_valid dropped mid-fill at word {word_idx}"

            if ready:
                dut.spi_ready.value = 1
                dut.spi_rdata.value = data_words[word_idx]
                dut.cpu_valid.value = 0
                ref.step(0, 0, 0, 0, 1, data_words[word_idx])
                await RisingEdge(dut.clk)
                word_idx += 1
                if word_idx >= WORDS_PER_LINE:
                    break
            else:
                dut.spi_ready.value = 0
                dut.cpu_valid.value = 0
                ref.step(0, 0, 0, 0, 0, 0)
                await RisingEdge(dut.clk)
            fill_cycles += 1

        await _wait(dut, ref, 2)
        assert dut.cpu_rdata.value == data_words[off], \
            f"Critical word mismatch: got {dut.cpu_rdata.value:#x}, expected {data_words[off]:#x}"
        assert dut.spi_valid.value == 0, "spi_valid should de-assert after fill completion"
        assert ref.state == "IDLE", f"ref should be IDLE, got {ref.state}"
        await _cleanup(dut, ref)

        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr
        dut.cpu_la_valid.value = 0
        ref.step(0, 0, 1, addr, 0, 0)
        await RisingEdge(dut.clk)
        await _wait(dut, ref, 2)
        assert dut.cpu_rdata.value == data_words[off], "Post-fill data mismatch"
        await _cleanup(dut, ref)

    if WORDS_PER_LINE == 1:
        for _ in range(50):
            tag = rng.randint(0, (1 << TAG_BITS) - 1)
            set_idx = rng.randint(0, SETS - 1)
            addr = make_addr(tag, set_idx, 0)
            data = rng.randint(0, 0xFFFFFFFF)

            dut.cpu_valid.value = 1
            dut.cpu_addr.value = addr
            dut.cpu_la_valid.value = 0
            dut.spi_ready.value = 0
            ref.step(0, 0, 1, addr, 0, 0)
            await RisingEdge(dut.clk)

            dut.cpu_valid.value = 0
            dut.spi_ready.value = 0
            ref.step(0, 0, 0, 0, 0, 0)
            await RisingEdge(dut.clk)

            dut.spi_ready.value = 1
            dut.spi_rdata.value = data
            ref.step(0, 0, 0, 0, 1, data)
            await RisingEdge(dut.clk)

            await _wait(dut, ref, 2)
            assert dut.cpu_rdata.value == data, "Single-word fill: data mismatch"
            await _cleanup(dut, ref)

    ref.save_snapshot()
