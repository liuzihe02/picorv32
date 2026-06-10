# test_memory.py — Structural memory integrity verification

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
async def test_memory(dut):
    """Verify structural RMW preserves unmodified way slices."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    num_test_sets = max(2, min(SETS, 10))
    test_sets = rng.sample(range(SETS), num_test_sets)

    for set_idx in test_sets:
        installed = {}
        for way_idx in range(WAYS):
            tag = rng.randint(0, (1 << TAG_BITS) - 1)
            while tag in [t for t, _ in installed.values()]:
                tag = rng.randint(0, (1 << TAG_BITS) - 1)
            data_words = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
            installed[way_idx] = (tag, data_words)
            await _fill_line(dut, ref, set_idx, tag, data_words)

        for _ in range(5):
            for way_idx in range(WAYS):
                old_tag, old_data = installed[way_idx]
                new_tag = rng.randint(0, (1 << TAG_BITS) - 1)
                while new_tag in [t for t, _ in installed.values()]:
                    new_tag = rng.randint(0, (1 << TAG_BITS) - 1)
                new_data = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
                installed[way_idx] = (new_tag, new_data)

                await _fill_line(dut, ref, set_idx, new_tag, new_data)

                for other_way in range(WAYS):
                    if other_way == way_idx:
                        continue
                    check_tag, check_data = installed[other_way]
                    addr = make_addr(check_tag, set_idx, 0)
                    dut.cpu_valid.value = 1
                    dut.cpu_addr.value = addr
                    dut.cpu_la_valid.value = 0
                    ref.step(0, 0, 1, addr, 0, 0)
                    await RisingEdge(dut.clk)
                    await _wait(dut, ref, 2)
                    assert dut.cpu_rdata.value == check_data[0], \
                        f"Filling way {way_idx} corrupted way {other_way} in set {set_idx}"
                    await _cleanup(dut, ref)

    test_set = test_sets[0]
    tags_used = []
    for i in range(10):
        tag = rng.randint(0, (1 << TAG_BITS) - 1)
        while tag in tags_used:
            tag = rng.randint(0, (1 << TAG_BITS) - 1)
        tags_used.append(tag)
        data = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]
        await _fill_line(dut, ref, test_set, tag, data)

        for way_idx in range(WAYS):
            stored_tag = ref._tags[test_set][way_idx]
            if stored_tag[0]:
                stored_data = ref._data[test_set][way_idx]
                addr = make_addr(stored_tag[1], test_set, 0)
                dut.cpu_valid.value = 1
                dut.cpu_addr.value = addr
                dut.cpu_la_valid.value = 0
                ref.step(0, 0, 1, addr, 0, 0)
                await RisingEdge(dut.clk)
                await _wait(dut, ref, 2)
                assert dut.cpu_rdata.value == stored_data[0], f"Way {way_idx} data corrupted"
                await _cleanup(dut, ref)

    ref.save_snapshot()
