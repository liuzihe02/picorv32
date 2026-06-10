# stim.py — Constrained random stimulus generators for icache verification

import random
from conftest import (
    SETS, WAYS, WORDS_PER_LINE,
    IDX_BITS, OFF_BITS, TAG_BITS,
    get_idx, get_tag, get_offset, make_addr,
)


def random_addr(rng, *, constrain_idx=None, constrain_tag=None,
                constrain_offset=None):
    """Generate a random 24-bit word-aligned byte address.

    Optionally pin one or more of {tag, index, offset} to specific values.
    """
    byte_off = 0  # always aligned (bits [1:0] = 0)

    if constrain_offset is not None:
        off = constrain_offset & ((1 << max(OFF_BITS, 1)) - 1)
    else:
        off = rng.randint(0, max(1, WORDS_PER_LINE) - 1)

    if constrain_idx is not None:
        idx = constrain_idx & ((1 << IDX_BITS) - 1)
    else:
        idx = rng.randint(0, SETS - 1)

    if constrain_tag is not None:
        tag = constrain_tag & ((1 << TAG_BITS) - 1)
    else:
        tag = rng.randint(0, (1 << TAG_BITS) - 1)

    return make_addr(tag, idx, off, byte_off)


def random_la_sequence(rng, *, same_index=True, constrain_idx=None):
    """Return (la_valid, la_addr, valid, addr) tuple.

    If same_index=True, both la_addr and addr share the same index
    (to exercise fast path). Otherwise they use different indices.

    The sequence is:
      - cpu_la_valid=1, cpu_la_addr=la_addr  on cycle N
      - cpu_valid=1,    cpu_addr=addr         on cycle N+1
    """
    idx = constrain_idx if constrain_idx is not None else rng.randint(0, SETS - 1)
    la_addr = random_addr(rng, constrain_idx=idx)

    if same_index:
        addr = random_addr(rng, constrain_idx=idx)
    else:
        other_idx = rng.randint(0, SETS - 1)
        while other_idx == idx:
            other_idx = rng.randint(0, SETS - 1)
        addr = random_addr(rng, constrain_idx=other_idx)

    return (1, la_addr, 1, addr)


def random_spi_gating(rng, p_stall=0.3):
    """Generator yielding True (ready) / False (stall) with Bernoulli(p_stall).

    Usage:
        gating = random_spi_gating(rng, p_stall=0.3)
        ready = next(gating)   # True or False
    """
    while True:
        yield rng.random() >= p_stall


async def fill_line(dut, ref, rng, set_idx, tag, data_words):
    """Perform a complete cache fill to a given set+tag by driving the DUT
    through a miss -> fill sequence. data_words is a list of 32-bit ints,
    one per word in the full line.

    Requires: DUT is in idle state, ref is synced.

    Returns the address used.
    """
    from conftest import WORDS_PER_LINE
    from cocotb.triggers import RisingEdge

    wpl = WORDS_PER_LINE
    addr = make_addr(tag, set_idx, 0)

    # Drive a miss request (no look-ahead for simplicity)
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr
    dut.cpu_la_valid.value = 0
    ref_out = ref.step(0, 0, 1, addr, 0, 0)
    await RisingEdge(dut.clk)

    # Now DUT should be in FILL, ref in FILL
    for wi in range(wpl):
        # Wait until DUT requests (spi_valid=1), then feed data
        # But actually we need to drive spi_ready and spi_rdata each cycle
        while True:
            # For each word, DUT asserts spi_valid=1, we provide data
            ready = True  # no gating for simplicity, or use p_stall=0
            dut.spi_ready.value = 1
            dut.spi_rdata.value = data_words[wi]
            dut.cpu_valid.value = 0  # release request
            ref_out = ref.step(0, 0, 0, 0, 1, data_words[wi])
            await RisingEdge(dut.clk)
            if wi == wpl - 1:
                break
            # Check if DUT accepted and moved to next word
            if dut.spi_valid.value == 1:
                break

    # After fill completion, back to idle
    assert ref.state == "IDLE", f"ref still in {ref.state} after fill"
    return addr


async def do_random_fill(dut, ref, rng, *, p_stall=0.0):
    """Random fill: pick a random address that misses, drive through SPI fill.

    Returns (addr, data_words) where data_words is the list of random words
    written to the line.
    """
    from cocotb.triggers import RisingEdge

    wpl = WORDS_PER_LINE
    tag = rng.randint(0, (1 << TAG_BITS) - 1)
    idx = rng.randint(0, SETS - 1)
    off = rng.randint(0, max(1, wpl) - 1)
    addr = make_addr(tag, idx, off)

    data_words = [rng.randint(0, 0xFFFFFFFF) for _ in range(wpl)]

    gating = random_spi_gating(rng, p_stall) if p_stall > 0 else None

    # Drive cpu_valid request
    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr, 0, 0)
    await RisingEdge(dut.clk)

    # Drive fill
    word_idx = 0
    while True:
        if gating:
            ready = next(gating)
        else:
            ready = True

        if ready:
            dut.spi_ready.value = 1
            dut.spi_rdata.value = data_words[word_idx]
            dut.cpu_valid.value = 0
            ref.step(0, 0, 0, 0, 1, data_words[word_idx])
            await RisingEdge(dut.clk)
            word_idx += 1
            if word_idx >= wpl:
                break
        else:
            dut.spi_ready.value = 0
            dut.cpu_valid.value = 0
            ref.step(0, 0, 0, 0, 0, 0)
            await RisingEdge(dut.clk)

    assert ref.state == "IDLE"
    return addr, data_words
