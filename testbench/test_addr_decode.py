# test_addr_decode.py — Address field partitioning verification

import cocotb
from cocotb.triggers import RisingEdge

from conftest import (
    clock_gen, reset, create_rng,
    SETS, WAYS, WORDS_PER_LINE, ITERATIONS,
    IDX_BITS, OFF_BITS, TAG_BITS,
    get_idx, get_tag, get_offset, make_addr,
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


async def _do_lookup(dut, ref, addr):
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr, 0, 0)
    await RisingEdge(dut.clk)
    await _wait(dut, ref, 2)


@cocotb.test()
async def test_addr_decode(dut):
    """Verify address partition into TAG, IDX, OFF, and byte fields."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    # Verify all index values produce hits to the correct set
    for iteration in range(min(ITERATIONS, SETS * 4)):
        set_idx = iteration % SETS
        tag = rng.randint(0, (1 << TAG_BITS) - 1)
        data_words = [rng.randint(0, 0xFFFFFFFF) for _ in range(WORDS_PER_LINE)]

        # Fill a line
        await _fill_line(dut, ref, set_idx, tag, data_words)

        # Verify hit on the same set_idx with same tag
        for off in range(max(1, WORDS_PER_LINE)):
            addr = make_addr(tag, set_idx, off)
            await _do_lookup(dut, ref, addr)
            assert dut.cpu_rdata.value == data_words[off], f"Data mismatch at offset {off}"
            await _cleanup(dut, ref)

        # Verify: same tag+index, different byte offset -> still same line (hit)
        for _ in range(3):
            byte_off = rng.randint(0, 3)
            addr_byte = make_addr(tag, set_idx, off=0, byte_off=byte_off)
            await _do_lookup(dut, ref, addr_byte)
            assert dut.cpu_rdata.value == data_words[0], f"Data mismatch with byte offset"
            await _cleanup(dut, ref)

        # Verify: different index and different tag produce different addr fields.
        other_idx = (set_idx + 1) % SETS
        addr_other = make_addr(tag, other_idx, off=0)
        assert get_idx(addr_other) == other_idx, "different index should decode correctly"
        assert get_tag(addr_other) == tag, "same tag should decode correctly"

        other_tag = (tag + 1) & ((1 << TAG_BITS) - 1)
        addr_diff = make_addr(other_tag, set_idx, off=0)
        assert get_idx(addr_diff) == set_idx, "same index should decode correctly"
        assert get_tag(addr_diff) == other_tag, "different tag should decode correctly"

    # Verify address field extraction directly with corner cases
    test_addrs = [
        0x000000,
        0xFFFFFF,
        0x000004,
        0x000008,
        0x00000C,
    ]
    for i in range(4):
        test_addrs.append(1 << i)
    for i in range(22, 24):
        test_addrs.append(1 << i)

    for addr in test_addrs:
        idx = get_idx(addr)
        tag = get_tag(addr)
        off = get_offset(addr)
        recomposed = make_addr(tag, idx, off, addr & 0x3)
        assert recomposed == addr, f"Address round-trip failed: {addr:#08x} -> tag={tag}, idx={idx}, off={off} -> {recomposed:#08x}"

        assert 0 <= idx < SETS, f"Index {idx} out of range [0, {SETS-1}]"
        assert 0 <= tag < (1 << TAG_BITS), f"Tag {tag} out of range"
        if WORDS_PER_LINE > 1:
            assert 0 <= off < WORDS_PER_LINE, f"Offset {off} out of range"
        else:
            assert off == 0, f"Offset should be 0 for WORDS_PER_LINE=1, got {off}"

    # --- Directed index sweep: hit every index value for cp_idx coverage ---
    for set_idx in range(SETS):
        tag = ((set_idx << 4) ^ 0xAA) & ((1 << TAG_BITS) - 1)
        data_words = [0xDEAD0000 | (set_idx << 8) | w for w in range(WORDS_PER_LINE)]
        await _fill_line(dut, ref, set_idx, tag, data_words)

    # --- Branch address patterns (bin_branch): jump across sets ---
    # Must drive back-to-back cpu_valid=1 with different addresses so
    # prev_addr != 0 and the transition is detected.
    for _ in range(min(ITERATIONS, 200)):
        idx_a = rng.randint(0, SETS - 1)
        idx_b = rng.randint(0, SETS - 1)
        while idx_b == idx_a:
            idx_b = rng.randint(0, SETS - 1)
        tag_a = rng.randint(0, (1 << TAG_BITS) - 1)
        tag_b = rng.randint(0, (1 << TAG_BITS) - 1)
        addr_a = make_addr(tag_a, idx_a, 0)
        addr_b = make_addr(tag_b, idx_b, 0)

        # Back-to-back requests: different index -> branch
        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr_a
        ref.step(0, 0, 1, addr_a, 0, 0)
        await RisingEdge(dut.clk)

        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr_b
        ref.step(0, 0, 1, addr_b, 0, 0)
        await RisingEdge(dut.clk)
        await _wait(dut, ref, 4)

    # --- Cross-line patterns (bin_cross_line): same set, different tag ---
    for _ in range(min(ITERATIONS, 100)):
        set_idx = rng.randint(0, SETS - 1)
        tag_a = rng.randint(0, (1 << TAG_BITS) - 1)
        tag_b = rng.randint(0, (1 << TAG_BITS) - 1)
        while tag_b == tag_a:
            tag_b = rng.randint(0, (1 << TAG_BITS) - 1)
        addr_a = make_addr(tag_a, set_idx, 0)
        addr_b = make_addr(tag_b, set_idx, 0)

        # Back-to-back requests: same index, different tag -> cross_line
        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr_a
        ref.step(0, 0, 1, addr_a, 0, 0)
        await RisingEdge(dut.clk)

        dut.cpu_valid.value = 1
        dut.cpu_addr.value = addr_b
        ref.step(0, 0, 1, addr_b, 0, 0)
        await RisingEdge(dut.clk)
        await _wait(dut, ref, 4)

    ref.save_snapshot()
