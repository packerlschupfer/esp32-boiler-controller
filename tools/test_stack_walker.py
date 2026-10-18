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



def test_branch_target_extraction():
    """Only objdump's printed 8-digit addresses count as branch targets.

    The immediates matter: `break 1, 15` and the `-1` in `beqi a8, -1, <addr>` must not be
    read as addresses, or half the image gains phantom branch targets and every function
    looks desynced.
    """
    bt = sw.Image._branch_target
    check(bt("j", "401ac068 <__ssvfiscanf_r+0x1da8>") == 0x401ac068, "j target extracted")
    check(bt("bbsi", "a8, 6, 401ac05d <f+0x1d9d>") == 0x401ac05d, "bbsi target extracted")
    check(bt("beqi", "a8, -1, 401ac03d <f+0x1d7d>") == 0x401ac03d,
          "beqi target extracted despite the -1 immediate")
    check(bt("beqz.n", "a3, 40081f94 <print_str+0x10>") == 0x40081f94, ".n suffix handled")
    check(bt("break", "1, 15") is None, "break's immediates are not a target")
    # The two defences above are independent and each masks the other in a mutation test,
    # so pin them separately. (a) the 8-digit requirement, isolated from the mnemonic filter:
    check(sw.BRANCH_TARGET_RE.match("15") is None, "a short immediate is not an address")
    check(sw.BRANCH_TARGET_RE.match("1a2b") is None, "a 4-digit immediate is not an address")
    check(sw.BRANCH_TARGET_RE.match("40081f94") is not None, "an 8-digit address matches")
    # (b) the mnemonic filter, isolated from the 8-digit requirement. `break` never prints an
    # address-shaped operand in practice; this pins that the filter alone would still reject it.
    check(bt("break", "1, 40081f94") is None,
          "break is rejected by the mnemonic filter even with an address-shaped operand")
    check(bt("l32r", "a9, 4018de84 <x+0xa48>") is None, "l32r is not a branch")
    check(bt("callx8", "a8") is None, "callx8 is not a branch")
    check(bt("add.n", "a9, a8, a9") is None, "arithmetic is not a branch")


def test_fiction_sites_real_print_str():
    """The hand-verified desync in print_str (IRAM, image 8ee1446).

    objdump prints `excw` at 40081f93 while the real instruction begins one byte later at
    40081f94, which the `beqz.n` at 40081f89 proves is an instruction boundary. Everything
    objdump prints from 40081f93 is therefore fiction.
    """
    instrs = [
        (0x40081f84, 3, "entry", "a1, 32"),
        (0x40081f87, 2, "mov.n", "a10, a2"),
        (0x40081f89, 2, "beqz.n", "a3, 40081f94 <print_str+0x10>"),
        (0x40081f8b, 3, "l32r", "a8, 40080588"),
        (0x40081f8e, 3, "callx8", "a8"),
        (0x40081f91, 2, "retw.n", ""),
        (0x40081f93, 3, "excw", ""),          # <- straddles the branch target 40081f94
        (0x40081f96, 2, "s32i.n", "a15, a0, 56"),
    ]
    fic = sw.Image._fiction_sites(instrs, {0x40081f94})
    check(0x40081f93 in fic, "the instruction straddling a branch target is fiction")
    check(0x40081f8e not in fic, "the real callx8 before the desync is NOT fiction")
    check(0x40081f84 not in fic, "the function's own entry is NOT fiction")
    # A cleanly decoded function must produce nothing, or 13.6% noise becomes 100%.
    clean = [(0x1000, 3, "entry", "a1, 32"), (0x1003, 3, "call8", "1100 <g>"),
             (0x1006, 2, "retw.n", "")]
    check(sw.Image._fiction_sites(clean, {0x1006}) == set(),
          "a cleanly decoded function yields no fiction sites")


def test_fiction_covers_the_ntptask_phantom():
    """The measured phantom: newlib scanf appearing to call a project C++ method.

    Two padding bytes after the `j` at 401ac01f, objdump decoded from 401ac022 over the
    real branch target 401ac024 and invented `call4 HealthMonitor::initialize()` at
    401ac028. That single edge put NTPTask at 3248 B against a true 1984 B. The call site
    must be reported as fiction, and a later proven boundary must clear the state so the
    rest of the function is not blamed.
    """
    instrs = [
        (0x401ac01f, 3, "j", "401ac068 <f+0x1da8>"),
        (0x401ac022, 3, "neg", "a0, a0"),      # <- straddles 401ac024
        (0x401ac025, 3, "addx2", "a15, a4, a5"),
        (0x401ac028, 3, "call4", "40134a30 <HealthMonitor::initialize()>"),
        (0x401ac03d, 3, "movi.n", "a6, 0"),    # a real branch target -> resync
        (0x401ac040, 3, "call8", "40100000 <real>"),
    ]
    targets = {0x401ac024, 0x401ac03d}
    fic = sw.Image._fiction_sites(instrs, targets)
    check(0x401ac028 in fic, "the phantom call4 site is flagged as fiction")
    check(0x401ac01f not in fic, "the j that precedes the padding is not itself fiction")
    check(0x401ac03d not in fic, "a printed address that IS a branch target resynchronises")
    check(0x401ac040 not in fic, "a real call after resync is not blamed")

print("branch-target extraction:")
test_branch_target_extraction()
print("fiction sites (decode desync):")
test_fiction_sites_real_print_str()
test_fiction_covers_the_ntptask_phantom()
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
