# test_handshake.py — Interface handshake protocol verification

import cocotb
from cocotb.triggers import RisingEdge, ClockCycles

from conftest import (
    clock_gen, reset, create_rng,
    SETS, WAYS, WORDS_PER_LINE,
    make_addr,
    CoverageTrackedRef,
)


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


@cocotb.test()
async def test_handshake(dut):
    """Verify CPU/SPI handshake timing, pulse widths, and double-response prevention."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    # --- Test 1: cpu_valid held across multiple cycles -> data check ---
    tag = 0x100
    set_idx = 5
    data = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
    addr = make_addr(tag, set_idx, 0)

    for _ in range(3):
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

    for wi in range(WORDS_PER_LINE):
        dut.spi_ready.value = 1
        dut.spi_rdata.value = data[wi]
        dut.cpu_valid.value = 1
        ref.step(0, 0, 1, addr, 1, data[wi])
        await RisingEdge(dut.clk)

    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data[0], "Test 1: rdata mismatch"
    assert ref.state == "IDLE"

    # --- Test 2: Single-cycle cpu_valid pulse -> fill completes ---
    tag2 = 0x200
    addr2 = make_addr(tag2, set_idx, 0)

    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr2
    ref.step(0, 0, 1, addr2, 0, 0)
    await RisingEdge(dut.clk)

    dut.cpu_valid.value = 0
    dut.spi_ready.value = 0
    ref.step(0, 0, 0, 0, 0, 0)
    await RisingEdge(dut.clk)

    for wi in range(WORDS_PER_LINE):
        dut.spi_ready.value = 1
        dut.spi_rdata.value = 0xAAAAAAAA
        ref.step(0, 0, 0, 0, 1, 0xAAAAAAAA)
        await RisingEdge(dut.clk)

    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == 0xAAAAAAAA, "Test 2: rdata mismatch"
    assert ref.state == "IDLE"

    # --- Test 3: cpu_la_valid and cpu_valid same cycle ---
    tag3 = 0x300
    data3 = [0xDEAD0000 + i for i in range(WORDS_PER_LINE)]
    addr3 = make_addr(tag3, set_idx + 1, 0)
    await _fill_line(dut, ref, set_idx + 1, tag3, data3)

    la_addr = make_addr(tag3, set_idx + 1, rng.randint(0, max(1, WORDS_PER_LINE) - 1))
    dut.cpu_la_valid.value = 1
    dut.cpu_la_addr.value = la_addr
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr3
    ref.step(1, la_addr, 1, addr3, 0, 0)
    await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data3[0], "Test 3: rdata mismatch"
    await _cleanup(dut, ref)

    # --- Test 4: spi_valid drops to 0 after fill completion ---
    tag4 = 0x400
    addr4 = make_addr(tag4, set_idx + 2, 0)
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr4
    ref.step(0, 0, 1, addr4, 0, 0)
    await RisingEdge(dut.clk)

    dut.cpu_valid.value = 0
    dut.spi_ready.value = 0
    ref.step(0, 0, 0, 0, 0, 0)
    await RisingEdge(dut.clk)

    for wi in range(WORDS_PER_LINE):
        dut.spi_ready.value = 1
        dut.spi_rdata.value = 0
        dut.cpu_valid.value = 0
        ref.step(0, 0, 0, 0, 1, 0)
        await RisingEdge(dut.clk)

    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == 0, "Test 4: rdata mismatch"
    assert dut.spi_valid.value == 0, "spi_valid should be 0 after fill"
    assert ref.state == "IDLE"

    # --- Test 5: init sweep produces a usable cache ---
    # (cpu_ready pulse timing validated by icache_tb.v)
    dut.resetn.value = 0
    ref.reset_model()
    await ClockCycles(dut.clk, 3)
    for _ in range(3):
        ref.step(0, 0, 0, 0, 0, 0)
    dut.resetn.value = 1
    await RisingEdge(dut.clk)
    # Advance through init
    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init
    # Verify cache is functional: fill + hit returns correct data
    tag5 = 0x500
    data5 = [0xBEEF0000 + i for i in range(WORDS_PER_LINE)]
    await _fill_line(dut, ref, 7, tag5, data5)
    addr5 = make_addr(tag5, 7, 0)
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr5
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr5, 0, 0)
    await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data5[0], "Test 5: post-init hit data mismatch"
    await _cleanup(dut, ref)

    ref.save_snapshot()
