# test_replace.py — Round-robin replacement policy verification

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
async def test_replace(dut):
    """Verify round-robin (or direct-mapped WAYS=1) victim selection."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    num_test_sets = max(4, min(SETS, 20))
    test_sets = rng.sample(range(SETS), num_test_sets)

    for set_idx in test_sets:
        installed = []
        for way_idx in range(WAYS):
            tag = rng.randint(0, (1 << TAG_BITS) - 1)
            while tag in [t for t, _ in installed]:
                tag = rng.randint(0, (1 << TAG_BITS) - 1)
            data_words = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
            installed.append((tag, data_words))

            await _fill_line(dut, ref, set_idx, tag, data_words)
            assert ref.state == "IDLE"

        for evict_step in range(WAYS):
            victim_idx = evict_step
            old_tag = installed[evict_step][0]
            old_data = installed[evict_step][1]

            new_tag = rng.randint(0, (1 << TAG_BITS) - 1)
            while new_tag in [t for t, _ in installed]:
                new_tag = rng.randint(0, (1 << TAG_BITS) - 1)
            new_data = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]

            await _fill_line(dut, ref, set_idx, new_tag, new_data)
            assert ref.state == "IDLE"

            for off in range(max(1, WORDS_PER_LINE)):
                addr = make_addr(new_tag, set_idx, off)
                dut.cpu_valid.value = 1
                dut.cpu_addr.value = addr
                dut.cpu_la_valid.value = 0
                ref.step(0, 0, 1, addr, 0, 0)
                await RisingEdge(dut.clk)
                await _wait(dut, ref, 2)
                assert dut.cpu_rdata.value == new_data[off], f"New entry data mismatch"
                await _cleanup(dut, ref)

            installed[evict_step] = (new_tag, new_data)

    if WAYS > 1 and len(test_sets) >= 2:
        set_a = test_sets[0]
        set_b = test_sets[1]

        for _ in range(3):
            tag = rng.randint(0, (1 << TAG_BITS) - 1)
            data = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
            await _fill_line(dut, ref, set_a, tag, data)

        tag_b = rng.randint(0, (1 << TAG_BITS) - 1)
        data_b = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
        await _fill_line(dut, ref, set_b, tag_b, data_b)

        addr_b = make_addr(tag_b, set_b, 0)
        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr_b
        dut.cpu_la_valid.value = 0
        ref.step(0, 0, 1, addr_b, 0, 0)
        await RisingEdge(dut.clk)
        await _wait(dut, ref, 2)
        assert dut.cpu_rdata.value == data_b[0], f"Set B fill should not be affected by set A fills"
        await _cleanup(dut, ref)

    ref.save_snapshot()
