# test_concurrency.py — Concurrent and back-to-back operations on different sets

import cocotb
from cocotb.triggers import RisingEdge

from conftest import (
    clock_gen, reset, create_rng,
    SETS, WAYS, WORDS_PER_LINE,
    TAG_BITS, make_addr,
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
async def test_concurrency(dut):
    """Verify fills and hits to different sets don't interfere."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    set_a = 10 % SETS
    set_b = 20 % SETS
    assert set_a != set_b

    data_a = [0xAA0000 + i for i in range(WORDS_PER_LINE)]
    data_b = [0xBB0000 + i for i in range(WORDS_PER_LINE)]
    tag_a = 0x100
    tag_b = 0x200

    await _fill_line(dut, ref, set_a, tag_a, data_a)
    await _fill_line(dut, ref, set_b, tag_b, data_b)

    addr_a0 = make_addr(tag_a, set_a, 0)
    addr_b0 = make_addr(tag_b, set_b, 0)

    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr_a0
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr_a0, 0, 0)
    await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data_a[0], "Set A data mismatch"
    await _cleanup(dut, ref)

    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr_b0
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr_b0, 0, 0)
    await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data_b[0], "Set B data mismatch"
    await _cleanup(dut, ref)

    new_tag_a = 0x300
    new_data_a = [0xCC0000 + i for i in range(WORDS_PER_LINE)]
    await _fill_line(dut, ref, set_a, new_tag_a, new_data_a)

    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr_b0
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr_b0, 0, 0)
    await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)
    assert dut.cpu_rdata.value == data_b[0], "Set B data should be intact"
    await _cleanup(dut, ref)

    for _ in range(3):
        tag = rng.randint(0, (1 << TAG_BITS) - 1)
        data = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
        addr = make_addr(tag, set_a, 0)

        await _fill_line(dut, ref, set_a, tag, data)

        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr
        dut.cpu_la_valid.value = 0
        ref.step(0, 0, 1, addr, 0, 0)
        await RisingEdge(dut.clk)
        await _wait(dut, ref, 2)
        assert dut.cpu_rdata.value == data[0], f"Post-fill hit should succeed"
        await _cleanup(dut, ref)

    ref.save_snapshot()

