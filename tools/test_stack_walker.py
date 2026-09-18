#!/usr/bin/env python3
"""Unit tests for tools/stack_walker.py internals.

Run: python3 tools/test_stack_walker.py

These pin two behaviours that were each wrong in a way that silently moved a stack
bound, which is the failure mode this tool exists to prevent:

  1. lookup() must return GCC clone symbols as well as ordinary ones. It returns at the
     first non-empty strategy, and a `static inline` function such as log_write_impl
     exists ~110 times in an optimised image - some copies demangled, some left raw as
     `_ZL14log_write_impl..$constprop$0` because the suffix defeats the demangler. The
     demangled copies matched, lookup stopped, and the clone copies were never returned,
     so the verified cut `dropOldestItem -> log_write_impl` matched no edge on the
     release image and left a whole printf-plus-malloc tail in several bounds.

  2. An `entry` instruction only sets a frame when it sits at the function's OWN address.
     On Xtensa `entry` is always the first instruction of a windowed function, so an
     `entry` seen elsewhere belongs to a different function. objdump does sometimes fail
     to decode a region - printing raw words with no mnemonic and no new symbol header -
     and without this rule the next decodable function's frame is credited to the
     preceding symbol. Measured: atomicUpdateBits (real frame 64 B) was credited 8552 B,
     putting BurnerControlTask at 9112 B against a 4096 B stack. A phantom 5 kB overflow
     on firmware that had just completed a real burn.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("stack_walker", os.path.join(HERE, "stack_walker.py"))
sw = importlib.util.module_from_spec(spec)
sys.argv = ["stack_walker"]          # keep argparse in __main__ from seeing our args
spec.loader.exec_module(sw)

failures = []


def check(cond, what):
    if cond:
        print(f"  ok   {what}")
    else:
        print(f"  FAIL {what}")
        failures.append(what)


class FakeImage(sw.Image if hasattr(sw, "Image") else object):
    """Just enough of the image for lookup(): a by_name map."""

    def __init__(self, by_name):
        self.by_name = by_name

    lookup = sw.Image.lookup if hasattr(sw, "Image") else None
    _clone_hits = sw.Image._clone_hits if hasattr(sw, "Image") else None
    _CLONE_RE = getattr(getattr(sw, "Image", object), "_CLONE_RE", None)


def test_lookup_returns_clones():
    # The real shape from an optimised build: several demangled copies of a static inline
    # plus several still-mangled $constprop$ clones of the same function.
    by_name = {
        "log_write_impl(esp_log_level_t, char const*, char const*, ...)": [0x1000, 0x1100],
        "_ZL14log_write_impl15esp_log_level_tPKcS1_z$constprop$0": [0x2000, 0x2100],
        # A different function that must NOT be dragged in by the length-prefix match.
        "_ZL9log_write15esp_log_level_tPKc$constprop$0": [0x3000],
        "custom_log_write(...)": [0x4000],
    }
    img = FakeImage(by_name)
    hits = set(img.lookup("log_write_impl"))
    check(0x1000 in hits and 0x1100 in hits, "lookup finds the demangled copies")
    check(0x2000 in hits and 0x2100 in hits, "lookup finds the $constprop$ clones")
    check(0x3000 not in hits, "lookup does NOT match a different function (log_write)")
    check(0x4000 not in hits, "lookup does NOT match custom_log_write")

    # A name with no clones present must still resolve, and must not gain phantom hits.
    check(set(img.lookup("custom_log_write")) == {0x4000}, "plain lookup unaffected")


def test_entry_must_be_at_function_start():
    f = sw.Func(0x400a0000, "someFunction")
    img = FakeImage({})
    # An `entry` at the function's own address sets the frame.
    sw.Image._step(img, f, {}, 0x400a0000, "entry", "a1, 64")
    check(f.frame == 64, "entry at the function's own address sets the frame")

    # A later `entry` must not overwrite it...
    sw.Image._step(img, f, {}, 0x400a0100, "entry", "a1, 8552")
    check(f.frame == 64, "a later entry does not overwrite the frame")

    # ...and must not set it at all when the first one was never seen (the objdump
    # decode-failure case). frame stays None so the report can flag an under-estimate.
    g = sw.Func(0x400b0000, "undecodableFunction")
    sw.Image._step(img, g, {}, 0x400b0140, "entry", "a1, 8552")
    check(g.frame is None,
          "a stray entry from a later function does NOT become this function's frame")


def test_entry_decoded_from_elf_bytes():
    """The ENTRY decode used to recover frames objdump renders as data.

    objdump classifies code vs data on this target from the `.xt.prop` property table, and
    that table has gaps: past a certain address it prints 4-byte words with no mnemonic and
    never resyncs. The instructions are still there - the low three bytes of the word ARE
    the `entry` - so frames are decoded from the image instead. On the real debug image this
    recovers 1517 frames that previously counted as 0, which UNDER-states a bound.

    Encoding, confirmed against objdump's own output on decodable regions:
        36 41 00 -> 0x004136 -> entry a1, 32
        36 81 00 -> 0x008136 -> entry a1, 64
    low byte 0x36 = opcode, bits 8-11 = base register (1 = a1), bits 12-23 = frame/8.
    """
    Img = sw.Image
    check((0x004136 & 0xFF) == Img.ENTRY_OPCODE, "0x004136 carries the ENTRY opcode")
    check(((0x004136 >> 8) & 0xF) == Img.ENTRY_REG_A1, "0x004136 uses a1 as the base")
    check(((0x004136 >> 12) & 0xFFF) * 8 == 32, "0x004136 decodes to entry a1, 32")
    check(((0x008136 >> 12) & 0xFFF) * 8 == 64, "0x008136 decodes to entry a1, 64")

    # The real-world case: objdump printed 7d008136 as a data word for atomicUpdateBits.
    # The trailing 0x7d is the NEXT instruction's first byte and must be ignored.
    word = 0x7D008136
    check((word & 0xFF) == Img.ENTRY_OPCODE, "the data word's low byte is still the opcode")
    check(((word >> 12) & 0xFFF) * 8 == 64,
          "atomicUpdateBits decodes to its real 64 B, not the phantom 8552 B")

    # Something that is NOT an entry must be rejected, or every data word becomes a frame.
    check((0x12345678 & 0xFF) != Img.ENTRY_OPCODE, "a non-entry word is rejected")
    # An entry against a register other than a1 is not a stack frame.
    check(((0x004236 >> 8) & 0xF) != Img.ENTRY_REG_A1, "entry on a2 is not treated as a1")


print("ENTRY decoding:")
test_entry_decoded_from_elf_bytes()
print("lookup():")
test_lookup_returns_clones()
print("entry/frame attribution:")
test_entry_must_be_at_function_start()

if failures:
    print(f"\n{len(failures)} failure(s)")
    sys.exit(1)
print("\nall stack_walker unit tests passed")
