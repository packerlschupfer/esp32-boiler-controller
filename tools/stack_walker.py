#!/usr/bin/env python3
"""
Static worst-case stack bound for ESP32 (Xtensa LX6) FreeRTOS tasks.

Walks the call graph of the linked firmware image from a task entry point and
reports the deepest reachable call chain, then converts it into a predicted peak
stack usage comparable with uxTaskGetStackHighWaterMark:

    predicted used = task prefix + call-chain depth + one interrupt frame

Why each piece exists (all measured from the binary at run time, not hardcoded):

  * call-chain depth  - sum of `entry a1, N` frames along the deepest path.
  * task prefix       - pxPortInitialiseStack reserves a coprocessor save area,
                        the task runs under vPortTaskWrapper (and, for tasks
                        started through TaskManager, TaskManager::safeTaskWrapper),
                        and the stack top is alignment-masked. All of it is
                        counted as "used" by the high-water mark.
  * interrupt frame   - _xt_lowint1 lowers the INTERRUPTED TASK's SP and saves
                        registers there before switching to the ISR stack, so an
                        interrupt landing at the deepest point adds that frame on
                        top. CONFIG_FREERTOS_ISR_STACKSIZE only holds the handler
                        body.

Edge resolution (a naive walker misses most of the real graph):

  * direct   call0/4/8/12 <target>, plus `j` tail calls to another function.
  * literal  `l32r aN, LIT` ... `callxN aN` - the -mlongcalls pattern. The word
             stored at LIT is read from the ELF load segments. Most "indirect"
             calls in this firmware are really this.
  * vtable   `call8 C::getInstance()` -> `l32i aV, a10, 0` -> `l32i aT, aV, OFF`
             -> `callxN aT`. Resolved through the `vtable for C` symbol: target
             is the word at vtable + 8 + OFF. This is how custom_log_write
             dispatches into Logger::logV.
  * manual   --edge CALLER=CALLEE for anything else (function pointers, handler
             tables). Unresolved indirect sites are always listed.

Three bounds are reported per task:

  worst case   every reachable path, including failure handling and one-time
               initialisation - answers "could error handling overflow this task?"
  nominal      failure-only and one-time code removed (tools/stack_walker.excludes)
  no-float     nominal without float formatting (_dtoa_r and the heap below it)

Float formatting is charged per CALL SITE, not per task. Every literal-pool string
is scanned for a %f/%e/%g conversion, and the search runs over a product graph of
(function, a-float-formatter-is-already-on-this-path); _dtoa_r may only be entered
once that bit is set. So a deep chain whose own format strings hold no float is not
charged the 600+ bytes of float formatting, even when the task formats floats
elsewhere. An observed peak above the no-float bound means the task HAS formatted a
float - an independent cross-check on the model.

A measured peak (NAME:STACK:FREE) must never exceed the nominal bound. If it does,
an exclusion removed a path that really ran, or a call edge is missing (exit 2).

Graph adjustments live in tools/stack_walker.excludes, each with a verified,
re-checkable reason. Only no-return panic/abort paths are excluded implicitly.

Recursion is handled on strongly connected components: cycles are searched
exactly (longest simple path, bounded budget), falling back to a conservative sum
of member frames. Either way each cycle is counted once - true recursion depth
cannot be bounded statically - and every cycle is reported.

Known limits: register tracking is linear (no control-flow merge), so a literal
loaded on one branch could be attributed to a callx on another; data decoded as
code can add spurious edges (both err toward over-estimation). Unresolved
indirect calls can hide real depth - if an observed peak exceeds the bound,
that is where to look.

objdump also loses instruction sync on alignment padding after an unconditional
`j`/`retw` and then prints instructions that do not exist, in 13.6% of this
image's functions. Those can decode as calls to real functions and add an
arbitrarily deep phantom subtree: one put NTPTask at 3248 B against a true
1984 B. Any such edge that carries the reported bound is now printed as a
PHANTOM-EDGE SUSPECT - a branch target that is not an instruction boundary
proves the desync. They are labelled, never dropped: the real instructions in a
mis-decoded span are ones objdump never emitted, and a span can only be proven
closed at a branch target, so dropping would risk UNDER-stating a bound.

Examples:
  tools/stack_walker.py BoilerTempControlTask:3584:1044
  tools/stack_walker.py --list-roots
  tools/stack_walker.py PumpControlModule::PumpControlTask:2560:928 --json
  tools/stack_walker.py MQTTTask::taskFunction --exclude 'QueueManager::ManagedQueue::dropLowestPriorityItem'
"""

import argparse
import glob
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

DEFAULT_ELF = ".pio/build/esp32dev_usb_debug_selective/firmware.elf"

# Functions that never return to the task in a way that matters for a stack
# bound: once reached, the system panics or reboots. Matched on the name before
# any '(' and with or without the @rom suffix.
NORETURN = {
    "abort", "__assert_func", "__assert", "esp_system_abort", "panic_abort",
    "esp_panic_handler", "xt_unhandled_exception", "vApplicationStackOverflowHook",
    "esp_restart_noos", "esp_restart", "_esp_error_check_failed",
    "__cxa_throw", "std::terminate", "__cxa_pure_virtual",
}

# Alignment loss at the top of a task stack: the heap hands out 4-byte aligned
# blocks and the port masks the top-of-stack pointer to 16 bytes.
ALIGN_LOSS = (4, 16)

# Cycles are searched exactly (longest simple path) up to this size and this many
# expansions; beyond either limit the bound falls back to summing every member
# frame. Both assume each cycle is traversed at most once - true recursion depth
# cannot be bounded statically, which is why cycles are always reported.
SCC_EXACT_MAX = 32
SCC_BUDGET = 500_000

# A printf conversion that pulls in float formatting (_dtoa_r and the heap below it).
FLOAT_SPEC_RE = re.compile(r"%[-+ #0-9.*]*(?:hh|h|l|ll|L|j|z|t)?[fFeEgGaA]")

FUNC_RE = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]+)\s+([a-z0-9_.]+)\s*(.*)$")
# Instructions after which control does not fall through, so the following bytes may be
# alignment padding rather than code - where objdump loses instruction sync.
UNCONDITIONAL = frozenset(("j", "jx", "ret", "ret.n", "retw", "retw.n", "ill"))
TARGET_RE = re.compile(r"^([0-9a-f]+)(?:\s+<(.+)>)?$")
L32R_RE = re.compile(r"^(a\d+),\s*([0-9a-f]+)")
# The last operand of a branch, when it is a full 8-digit code address rather than an
# immediate. Requiring 8 digits is what keeps `break 1, 15` and `beqi a8, -1, ...`'s
# immediates out: only objdump's printed addresses match.
BRANCH_TARGET_RE = re.compile(r"^([0-9a-f]{8})(?:\s+<.+>)?$")
GETINSTANCE_RE = re.compile(r"^(.+)::getInstance\(\)$")

# Mnemonics whose first operand is NOT a destination register.
NO_DEST_PREFIXES = ("s32", "s16", "s8", "wsr", "xsr", "ret", "memw", "nop",
                    "break", "ill", "rfe", "rfi", "rfde", "rfwo", "rfwu",
                    "waiti", "j", "call", "entry", "ssi", "ssx")


def die(msg):
    sys.exit(f"stack_walker: {msg}")


def find_tool(name):
    pattern = os.path.expanduser(
        f"~/.platformio/packages/toolchain-xtensa-esp32*/bin/xtensa-esp32-elf-{name}")
    hits = sorted(glob.glob(pattern))
    if not hits:
        die(f"cannot find xtensa-esp32-elf-{name} under ~/.platformio/packages")
    return hits[-1]


def disassemble(elf):
    st = os.stat(elf)
    key = hashlib.sha1(
        f"{os.path.abspath(elf)}:{st.st_mtime_ns}:{st.st_size}".encode()).hexdigest()[:16]
    cache = f"/tmp/stack_walker_{key}.dis"
    if not os.path.exists(cache):
        tmp = cache + ".tmp"
        with open(tmp, "w") as out:
            subprocess.run([find_tool("objdump"), "-d", "--demangle", elf],
                           stdout=out, stderr=subprocess.DEVNULL, check=True)
        os.replace(tmp, cache)
    return cache


class ElfImage:
    """Minimal ELF32-LE reader: maps load segments so literal words can be read."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 1 or self.data[5] != 1:
            die(f"{path} is not a little-endian ELF32 image")
        phoff = struct.unpack_from("<I", self.data, 0x1C)[0]
        phentsize, phnum = struct.unpack_from("<HH", self.data, 0x2A)
        self.segments = []
        for i in range(phnum):
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, _ = struct.unpack_from(
                "<IIIIII", self.data, phoff + i * phentsize)
            if p_type == 1 and p_filesz:
                self.segments.append((p_vaddr, p_vaddr + p_filesz, p_offset))
                if p_paddr != p_vaddr:
                    self.segments.append((p_paddr, p_paddr + p_filesz, p_offset))

    def read32(self, addr):
        for lo, hi, off in self.segments:
            if lo <= addr <= hi - 4:
                return struct.unpack_from("<I", self.data, off + (addr - lo))[0]
        return None

    def cstring(self, addr, limit=200):
        """NUL-terminated string at a virtual address, or None."""
        for lo, hi, off in self.segments:
            if lo <= addr < hi:
                raw = self.data[off + (addr - lo): off + (addr - lo) + limit]
                end = raw.find(b"\0")
                if end <= 0:
                    return None
                text = raw[:end]
                if any(b < 9 or (13 < b < 32) for b in text):
                    return None
                return text.decode("utf-8", "replace")
        return None


def data_symbols(elf):
    """name -> address for data symbols (vtables live in rodata, not in the listing)."""
    out = subprocess.run([find_tool("nm"), "-C", elf], capture_output=True,
                         text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and re.fullmatch(r"[0-9a-f]{8}", parts[0]):
            syms.setdefault(parts[2], int(parts[0], 16))
    return syms


class Func:
    __slots__ = ("addr", "name", "frame", "edges", "unresolved", "resolved", "pending",
                 "insns", "desync", "fiction_edges")

    def __init__(self, addr, name):
        self.addr = addr
        self.name = name
        self.frame = None
        self.edges = set()        # target addresses
        self.unresolved = []      # (site, reason)
        self.resolved = []        # (site, how, target_addr)
        self.pending = []         # (site, how, target_addr) - checked after all parsing
        self.insns = None         # kept only for model-extraction functions
        self.desync = ()          # branch targets that are not instruction boundaries
        self.fiction_edges = set()  # targets whose call site is in a mis-decoded span


def base_name(name):
    return name.split("(", 1)[0]


def to_int(tok):
    """Parse an objdump immediate as a signed 32-bit value.

    objdump prints some negatives already signed ("-96") and others as unsigned
    hex ("0xffffff00"). Only the latter may be wrapped: a negative Python int
    also has bit 31 set, so wrapping it again would add 2**32.
    """
    v = int(tok, 0)
    return v - (1 << 32) if v >= 0 and v & 0x80000000 else v


class Image:
    MODEL_FUNCS = {"_xt_lowint1", "pxPortInitialiseStack"}

    def __init__(self, elf, rom_elf=None):
        self.elf = ElfImage(elf)
        # Must exist before the first parse: read32() consults the ROM image for
        # literals that point into mask ROM.
        self.rom = ElfImage(rom_elf) if rom_elf else None
        self.funcs = {}           # addr -> Func
        self.by_name = {}         # name -> [addr]
        self.float_formats = {}   # addr -> {format strings pulling in _dtoa_r}
        self.vtables = {}         # class -> vtable address
        for name, addr in data_symbols(elf).items():
            if name.startswith("vtable for "):
                self.vtables[name[len("vtable for "):]] = addr
        # ROM first: app literals that point into mask ROM are resolved while the
        # app is parsed, so ROM functions must already be registered. ROM never
        # calls into the app directly, so the reverse order is never needed.
        if rom_elf:
            self._parse(disassemble(rom_elf), suffix="@rom")
        self._parse(disassemble(elf), suffix="")
        self._resolve_pending()
        self._recover_missing_frames()

    # Xtensa ENTRY, decoded straight from the ELF bytes.
    #   36 41 00  ->  0x004136  ->  entry a1, 32
    #   36 81 00  ->  0x008136  ->  entry a1, 64
    # low byte 0x36 is the opcode, bits 8-11 are the base register (1 = a1), and bits
    # 12-23 are the frame size in 8-byte units.
    ENTRY_OPCODE = 0x36
    ENTRY_REG_A1 = 1

    def _recover_missing_frames(self):
        """Read `entry` from the image for functions objdump rendered as data.

        objdump classifies code vs data on this target using the `.xt.prop` property
        table, and that table has gaps: from one address onward it prints 4-byte words with
        no mnemonic and never resyncs, so those functions get no frame at all. The
        instructions are present - the low three bytes of the word objdump prints ARE the
        `entry` - so decode them directly rather than trusting the classification.

        Frame 0 UNDER-states a bound, which is the dangerous direction, and on the current
        debug image this recovers BurnerRequestManager::atomicUpdateBits (64 B),
        getBurnerRequestEventGroup, esp_mqtt_client_publish and tcp_poll among others.
        Functions that legitimately have no `entry` - hand-written assembly such as
        _xt_context_restore, xthal_* and _frxt_dispatch, which are not windowed - keep a
        None frame, which is correct for them and still reported.
        """
        self.recovered_frames = {}
        for addr, f in self.funcs.items():
            if f.frame is not None or "@rom" in f.name:
                continue
            word = self.elf.read32(addr)
            if word is None:
                continue
            if (word & 0xFF) != self.ENTRY_OPCODE:
                continue
            if ((word >> 8) & 0xF) != self.ENTRY_REG_A1:
                continue
            f.frame = ((word >> 12) & 0xFFF) * 8
            self.recovered_frames[addr] = f.frame

    def _resolve_pending(self):
        # Indirect targets are only checked once every function is registered. A
        # callee that sits later in address order than its caller is not known
        # while the caller is being parsed - custom_log_write (0x400d33c8)
        # dispatches into Logger::logV (0x400d4390), for example.
        for f in self.funcs.values():
            for site, how, target in f.pending:
                if target in self.funcs:
                    f.edges.add(target)
                    f.resolved.append((site, how, target))
                else:
                    f.unresolved.append((site, f"{how} target {target:#010x} is not a function start"))
            f.pending = []

    def read32(self, addr):
        v = self.elf.read32(addr)
        if v is None and self.rom is not None:
            v = self.rom.read32(addr)
        return v

    @staticmethod
    def _branch_target(mnem, ops):
        """The code address a direct branch/jump transfers to, or None.

        Used only to find decode desync (see _close_function), never to add call edges.
        """
        if mnem == "break" or mnem.startswith("break"):
            return None
        if not (mnem.startswith("b") or mnem == "j" or mnem.startswith("loop")):
            return None
        m = BRANCH_TARGET_RE.match(ops.rsplit(",", 1)[-1].strip())
        return int(m.group(1), 16) if m else None

    @staticmethod
    def _fiction_sites(instrs, targets):
        """Addresses objdump printed that cannot be real instructions.

        On Xtensa every branch target IS an instruction boundary. So when objdump prints
        an instruction that STRADDLES a branch target, its decode is provably misaligned:
        it lost instruction sync - typically on alignment padding after an unconditional
        `j`/`retw` - and everything it prints from there is fiction until it happens to
        resynchronise, which we can only prove when a printed address IS a branch target.

        Measured, in newlib's __ssvfiscanf_r: two padding bytes after a `j` at 401ac01f,
        objdump decoded from 401ac022 straight over the real branch target 401ac024 and
        manufactured `call4 HealthMonitor::initialize()` at 401ac028 plus a `call0` to an
        address past `_etext`. The walker believed the first one, and NTPTask's bound read
        3248 B against a true 1984 B - 800 B invented out of padding.

        Real and common: 13.6% of the 7493 functions in this image contain at least one
        such spot (worst: __ssvfiscanf_r 121, _vfprintf_r 117, BoilerTempControlTask 37).
        Hand-verified in print_str, where objdump prints `excw` at 40081f93 while the real
        instruction begins at the branch target 40081f94.

        Returns the set of printed addresses inside such a span. Used to LABEL edges, not
        to drop them by default: the real instructions in a fiction span are ones objdump
        never emitted, so their edges were always missing, and a span that resynchronises
        somewhere other than a branch target cannot be proven closed - dropping there
        could discard a real edge and UNDER-state a bound, the one unsafe direction.
        """
        fiction, out = False, set()
        for i, (site, length, mnem, _ops) in enumerate(instrs):
            if fiction:
                if site in targets:          # proven resynchronised
                    fiction = False
                else:
                    out.add(site)
                    continue
            # A printed instruction that steps over a branch target is proof of misalignment.
            if any(site < t < site + length for t in targets):
                out.add(site)
                fiction = True
        return out

    @classmethod
    def _close_function(cls, cur, instrs, targets):
        if not (cur and instrs):
            return
        starts = {s for s, _l, _m, _o in instrs}
        lo, hi = cur.addr, max(starts)
        cur.desync = tuple(sorted(t for t in targets if lo <= t <= hi and t not in starts))
        return cls._fiction_sites(instrs, targets)

    def _parse(self, listing, suffix):
        # Instructions are buffered per function rather than stepped as they are read,
        # because deciding whether a printed instruction is real needs the function's
        # forward branch targets too - see _fiction_sites.
        cur, instrs, targets = None, [], set()

        def close():
            fiction = self._close_function(cur, instrs, targets) or set()
            if cur is None:
                return
            regs = {}
            for site, _length, mnem, ops in instrs:
                if cur.insns is not None and len(cur.insns) < 60:
                    cur.insns.append((mnem, ops))
                self._step(cur, regs, site, mnem, ops, site in fiction)

        with open(listing, errors="replace") as f:
            for line in f:
                m = FUNC_RE.match(line.rstrip("\n"))
                if m:
                    close()
                    instrs, targets = [], set()
                    addr = int(m.group(1), 16)
                    name = m.group(2) + suffix
                    if addr in self.funcs:        # ROM and app never overlap; be safe
                        cur = None
                        continue
                    cur = Func(addr, name)
                    if base_name(m.group(2)) in self.MODEL_FUNCS and not suffix:
                        cur.insns = []
                    self.funcs[addr] = cur
                    self.by_name.setdefault(name, []).append(addr)
                    continue
                if cur is None:
                    continue
                m = INSN_RE.match(line)
                if not m:
                    continue
                site = int(m.group(1), 16)
                mnem, ops = m.group(3), m.group(4).strip()
                instrs.append((site, len(m.group(2)) // 2, mnem, ops))
                bt = self._branch_target(mnem, ops)
                if bt is not None:
                    targets.add(bt)
        close()   # last function in the listing

    def _step(self, cur, regs, site, mnem, ops, fiction=False):
        if mnem == "entry":
            parts = [p.strip() for p in ops.split(",")]
            # `site == cur.addr` is REQUIRED, not a sanity check. On Xtensa `entry` is always
            # the FIRST instruction of a windowed function, so an `entry` anywhere else
            # belongs to a different function - and objdump does sometimes fail to decode a
            # region, printing raw words with no mnemonic and no new symbol header. Without
            # this guard the next decodable function's entry is silently attributed to the
            # preceding symbol.
            #
            # Measured cost of not having it: BurnerRequestManager::atomicUpdateBits (real
            # frame 64 B, hand-decoded from `36 81 00` = entry a1, 8*8) was credited with
            # 8552 B from a later unsymbolised function, which put BurnerControlTask's bound
            # at 9112 B against a 4096 B stack - a 5 kB phantom overflow on firmware that
            # had just run a real burn. It can err the other way too: a smaller stray entry
            # would UNDER-state a frame, which is the dangerous direction.
            if len(parts) == 2 and parts[0] == "a1" and cur.frame is None \
                    and site == cur.addr:
                cur.frame = int(parts[1], 0)
            return

        if mnem == "l32r":
            m = L32R_RE.match(ops)
            if m:
                value = self.read32(int(m.group(2), 16))
                regs[m.group(1)] = ("lit", value)
                # Format strings arrive through the literal pool exactly like
                # function pointers, so the same load tells us whether this
                # function can ask printf for a float conversion.
                if value is not None:
                    text = self.elf.cstring(value) or (self.rom.cstring(value) if self.rom else None)
                    if text and FLOAT_SPEC_RE.search(text):
                        self.float_formats.setdefault(cur.addr, set()).add(text[:80])
            return

        call = re.fullmatch(r"call(0|4|8|12)", mnem)
        callx = re.fullmatch(r"callx(0|4|8|12)", mnem)
        if call or callx:
            window = int((call or callx).group(1))
            if call:
                t = TARGET_RE.match(ops)
                if t:
                    target = int(t.group(1), 16)
                    cur.edges.add(target)
                    if fiction:
                        cur.fiction_edges.add(target)
                    ret_reg = {0: "a2", 4: "a6", 8: "a10", 12: "a14"}[window]
                    name = t.group(2) or ""
                    gi = GETINSTANCE_RE.match(name)
                    self._clobber(regs, window)
                    if gi:
                        regs[ret_reg] = ("obj", gi.group(1))
                    return
            else:
                self._resolve_indirect(cur, regs, site, ops.strip())
            self._clobber(regs, window)
            return

        if mnem == "j":
            t = TARGET_RE.match(ops)
            if t and t.group(2) and "+" not in t.group(2):
                target = int(t.group(1), 16)
                if target != cur.addr:
                    cur.edges.add(target)     # tail call into another function
                    if fiction:
                        cur.fiction_edges.add(target)
            return
        if mnem == "jx":
            self._resolve_indirect(cur, regs, site, ops.strip())
            return

        parts = [p.strip() for p in ops.split(",")]
        if mnem in ("l32i", "l32i.n") and len(parts) == 3:
            dst, src, off = parts[0], parts[1], to_int(parts[2])
            state = regs.get(src)
            if state and state[0] == "obj" and off == 0:
                regs[dst] = ("vptr", state[1])
            elif state and state[0] == "vptr":
                regs[dst] = ("vslot", state[1], off)
            else:
                regs.pop(dst, None)
            return
        if mnem in ("mov", "mov.n") and len(parts) == 2:
            if parts[1] in regs:
                regs[parts[0]] = regs[parts[1]]
            else:
                regs.pop(parts[0], None)
            return
        if mnem == "or" and len(parts) == 3 and parts[1] == parts[2]:
            if parts[1] in regs:
                regs[parts[0]] = regs[parts[1]]
            else:
                regs.pop(parts[0], None)
            return

        if mnem.startswith("b") and mnem != "break":
            return                             # branches write no register
        if mnem.startswith(NO_DEST_PREFIXES):
            return
        if parts and re.fullmatch(r"a\d+", parts[0]):
            regs.pop(parts[0], None)

    @staticmethod
    def _clobber(regs, window):
        # Windowed call: the callee owns a{window}..a15 of the caller. call0 uses
        # the fixed ABI, where only a12-a15 survive.
        for r in list(regs):
            n = int(r[1:])
            if (window == 0 and n not in (1, 12, 13, 14, 15)) or (window and n >= window):
                del regs[r]

    def _resolve_indirect(self, cur, regs, site, reg):
        state = regs.get(reg)
        if not state:
            cur.unresolved.append((site, f"{reg} not from a literal or vtable (true indirect)"))
            return
        if state[0] == "lit":
            if state[1] is None:
                cur.unresolved.append((site, "literal unreadable"))
            else:
                cur.pending.append((site, "literal", state[1]))
            return
        if state[0] == "vslot":
            cls, off = state[1], state[2]
            vt = self.vtables.get(cls)
            if vt is None:
                cur.unresolved.append((site, f"no vtable symbol for {cls}"))
                return
            target = self.read32(vt + 8 + off)
            if target is None:
                cur.unresolved.append((site, f"vtable {cls}+{off} unreadable"))
            else:
                cur.pending.append((site, f"vtable {cls}+{off}", target))
            return
        cur.unresolved.append((site, f"{reg} holds {state[0]}, not a call target"))

    # ------------------------------------------------------------------ lookup

    # GCC clone suffixes ($constprop$N, .isra.N, $part$N). An optimised build emits a
    # separate copy of a function per specialisation, and objdump's demangler FAILS on the
    # ones carrying a "$constprop$" suffix, leaving them raw - e.g.
    # `_ZL14log_write_impl15esp_log_level_tPKcS1_z$constprop$0` beside a perfectly
    # demangled `log_write_impl(esp_log_level_t, char const*, char const*, ...)`.
    #
    # This matters because lookup() returns at the FIRST non-empty strategy. A `static
    # inline` function like log_write_impl exists ~110 times in the release image; the
    # demangled copies matched, lookup returned those and stopped, and the clone copies were
    # never returned. So the verified cut `dropOldestItem -> log_write_impl` matched no edge
    # on release - dropOldestItem calls a CLONE - which left the whole printf-plus-malloc
    # tail in several bounds and made release look ~1.5 kB worse per affected task than it is.
    #
    # Deliberately conservative: a mangled symbol is only considered when it carries a clone
    # marker AND contains the Itanium length-prefixed component "<len><name>", so the length
    # must agree too. That cannot pull in an unrelated function, which matters because these
    # addresses feed `cut:` - and a cut matching MORE would SHRINK a bound.
    _CLONE_RE = re.compile(r"\$(?:constprop|part|sra)\$\d+|\.(?:constprop|isra|part|cold)\.\d+")

    def _clone_hits(self, name):
        bare = name.replace("@rom", "")
        if bare.startswith("_Z"):
            return []
        token = f"{len(bare)}{bare}"
        out = []
        for n, addrs in self.by_name.items():
            raw = n.replace("@rom", "")
            if not self._CLONE_RE.search(raw):
                continue
            if raw.startswith("_Z"):
                if token in raw:                       # still-mangled clone
                    out.extend(addrs)
            elif base_name(self._CLONE_RE.sub("", raw)).rstrip(" []clone") == bare:
                out.extend(addrs)                      # demangled clone
        return out

    def lookup(self, name):
        if name in self.by_name:
            return self.by_name[name]
        # Prefer "name(params)" exactly: a bare base-name match would also hit
        # nested symbols such as "Task(void*)::{lambda()#1}::operator()() const".
        plain = re.compile(re.escape(name) + r"\([^()]*\)(?: const)?(?:@rom)?$")
        hits = [a for n, addrs in self.by_name.items() for a in addrs if plain.match(n)]
        hits += self._clone_hits(name)
        if hits:
            return hits
        bare = name.replace("@rom", "")
        return [a for n, addrs in self.by_name.items() for a in addrs
                if base_name(n.replace("@rom", "")) == bare]

    def frame(self, addr):
        f = self.funcs.get(addr)
        return (f.frame or 0) if f else 0

    def name(self, addr):
        f = self.funcs.get(addr)
        return f.name if f else f"?{addr:#010x}"

    # ------------------------------------------------------------------- model

    def model(self, task_manager_wrapper):
        notes = []
        irq = None
        f = self._first("_xt_lowint1")
        if f and f.insns:
            total = 0
            for mnem, ops in f.insns[:8]:
                parts = [p.strip() for p in ops.split(",")]
                if mnem in ("addmi", "addi") and len(parts) == 3 and parts[0] == parts[1] == "a1":
                    total += to_int(parts[2])
            if total < 0:
                irq = -total
        if irq is None:
            irq = 192
            notes.append("interrupt frame not parsed from _xt_lowint1; assumed 192 B")

        cp = None
        f = self._first("pxPortInitialiseStack")
        if f and f.insns:
            for mnem, ops in f.insns:
                parts = [p.strip() for p in ops.split(",")]
                if mnem == "addi" and len(parts) == 3 and parts[1] == "a2" and parts[0] != "a2":
                    v = to_int(parts[2])
                    if v < 0:
                        cp = -v
                        break
        if cp is None:
            cp = 96
            notes.append("coprocessor save area not parsed from pxPortInitialiseStack; assumed 96 B")

        wrappers = []
        for w in ("vPortTaskWrapper", "TaskManager::safeTaskWrapper"):
            if w == "TaskManager::safeTaskWrapper" and not task_manager_wrapper:
                continue
            fw = self._first(w)
            if fw and fw.frame:
                wrappers.append((w, fw.frame))
            else:
                notes.append(f"{w} not found; not counted")
        return {"irq": irq, "coprocessor": cp, "wrappers": wrappers,
                "align": ALIGN_LOSS, "notes": notes}

    def _first(self, name):
        addrs = self.lookup(name)
        return self.funcs.get(addrs[0]) if addrs else None


# ---------------------------------------------------------------------- walk

def reachable(img, root, excluded, cut_edges=frozenset()):
    """Plain address reachability - used for guard conditions and reporting."""
    seen, todo = set(), [root]
    while todo:
        a = todo.pop()
        if a in seen or a in excluded:
            continue
        seen.add(a)
        f = img.funcs.get(a)
        if f:
            todo.extend(t for t in f.edges if (a, t) not in cut_edges)
    return seen


def tarjan(nodes, succ):
    index, low, onstack, stack, sccs, counter = {}, {}, set(), [], [], [0]

    def push(v):
        index[v] = low[v] = counter[0]
        counter[0] += 1
        stack.append(v)
        onstack.add(v)

    for v0 in nodes:
        if v0 in index:
            continue
        push(v0)
        work = [(v0, iter(succ(v0)))]
        while work:
            v, it = work[-1]
            for w in it:
                if w not in index:
                    push(w)
                    work.append((w, iter(succ(w))))
                    break
                if w in onstack:
                    low[v] = min(low[v], index[w])
            else:
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    comp = []
                    while True:
                        w = stack.pop()
                        onstack.discard(w)
                        comp.append(w)
                        if w == v:
                            break
                    sccs.append(comp)
    return sccs   # reverse topological order: callees before callers


def longest_path(succ, frame_of, root):
    """Longest frame-sum path from `root` over any directed graph.

    Node ids are arbitrary hashables, which is what lets the caller walk a
    product graph (function, state) rather than plain addresses. Cycles are
    searched exactly inside small SCCs and summed conservatively in large ones;
    either way a cycle is counted once.
    """
    seen, todo = set(), [root]
    while todo:
        n = todo.pop()
        if n in seen:
            continue
        seen.add(n)
        todo.extend(succ(n))

    def succ_in(n):
        return [m for m in succ(n) if m in seen]

    val, cont, recursion = {}, {}, []
    for comp in tarjan([root], succ_in):
        members = set(comp)
        cyclic = len(comp) > 1 or comp[0] in succ_in(comp[0])

        def exit_of(m):
            best, node = 0, None
            for w in succ_in(m):
                if w not in members and val[w] > best:
                    best, node = val[w], w
            return best, node

        if not cyclic:
            n = comp[0]
            b, node = exit_of(n)
            val[n] = frame_of(n) + b
            cont[n] = ([node] if node is not None else [], node)
            continue

        exact = None
        if len(comp) <= SCC_EXACT_MAX:
            exits = {m: exit_of(m) for m in comp}
            inner = {m: [w for w in succ_in(m) if w in members] for m in comp}
            budget = SCC_BUDGET
            exact = {}
            for start in comp:
                best = (-1, (start,), None)
                stack = [(start, (start,), frame_of(start))]
                while stack:
                    budget -= 1
                    if budget < 0:
                        exact = None
                        break
                    m, p, acc = stack.pop()
                    total = acc + exits[m][0]
                    if total > best[0]:
                        best = (total, p, exits[m][1])
                    for w in inner[m]:
                        if w not in p:
                            stack.append((w, p + (w,), acc + frame_of(w)))
                if exact is None:
                    break
                exact[start] = best
        if exact is not None:
            for start, (total, p, exit_node) in exact.items():
                val[start] = total
                cont[start] = (list(p[1:]) + ([exit_node] if exit_node is not None else []), exit_node)
            recursion.append((comp, "exact"))
        else:
            total = sum(frame_of(m) for m in comp)
            best, node = max((exit_of(m) for m in comp), key=lambda e: e[0])
            for m in comp:
                val[m] = total + best
                cont[m] = ([("scc", len(comp), total)] + ([node] if node is not None else []), node)
            recursion.append((comp, "conservative"))

    path, cur = [root], root
    while True:
        seq, nxt = cont.get(cur, ([], None))
        path.extend(seq)
        if nxt is None or nxt == cur:
            break
        cur = nxt
    return val.get(root, 0), path, seen, recursion


def deepest(img, root, excluded, cut_edges=frozenset(),
            float_sinks=frozenset(), float_formatters=frozenset()):
    """Deepest chain from `root`, attributing float formatting per call site.

    Walks the product graph (function, seen-a-float-formatter). Entering a float
    sink (_dtoa_r and below) is only allowed once some function holding a %f-style
    format string is already on the path, so a deep chain whose own format strings
    contain no float is not charged for float formatting. Passing an empty
    `float_formatters` makes the bit unreachable, which yields the no-float bound.
    """
    def out_edges(a):
        f = img.funcs.get(a)
        if not f:
            return ()
        return [t for t in f.edges if t not in excluded and (a, t) not in cut_edges]

    def succ(node):
        a, seen_float = node
        nxt = []
        for m in out_edges(a):
            flag = seen_float or (m in float_formatters)
            if m in float_sinks and not flag:
                continue
            nxt.append((m, flag))
        return nxt

    root_node = (root, root in float_formatters)
    depth, path, prod_nodes, recursion = longest_path(
        succ, lambda n: img.frame(n[0]), root_node)
    nodes = {n[0] for n in prod_nodes}
    recursion = [([img.name(m[0]) for m in comp], kind) for comp, kind in recursion]
    return depth, path, nodes, recursion


def parse_root(spec):
    """NAME | NAME:STACK | NAME:STACK:FREE

    NAME:STACK (no measured peak) answers the pre-flash question: does the static
    bound fit a configured stack? That is the only form available for a build that
    has never run - a release image, or any change not yet flashed - and passing a
    fake FREE of 0 instead would read as "measured used == stack" and trip the
    bound-violated check. Names can contain ':' (C++ scope), so a trailing field is
    only treated as a number when it is entirely digits."""
    parts = spec.rsplit(":", 2)
    if len(parts) == 3 and parts[1].isdigit() and parts[2].isdigit():
        return parts[0], int(parts[1]), int(parts[2])
    head, sep, tail = spec.rpartition(":")
    if sep and tail.isdigit() and not head.endswith(":"):
        return head, int(tail), None
    return spec, None, None


def summarise(img, root, excluded, model, cut_edges=frozenset(),
              float_sinks=frozenset(), float_formatters=frozenset()):
    depth, path, nodes, recursion = deepest(img, root, excluded, cut_edges,
                                            float_sinks, float_formatters)
    fixed = model["coprocessor"] + sum(fr for _, fr in model["wrappers"])
    lo = depth + fixed + model["align"][0] + model["irq"]
    hi = depth + fixed + model["align"][1] + model["irq"]
    rows, cum = [], 0
    for step in path:
        if step and step[0] == "scc":                 # conservative recursion marker
            cum += step[2]
            rows.append({"frame": step[2], "cumulative": cum, "addr": None,
                         "function": f"[recursive group of {step[1]} functions, conservative]"})
            continue
        addr = step[0]
        fr = img.frame(addr)
        cum += fr
        f = img.funcs.get(addr)
        rows.append({"frame": fr, "cumulative": cum, "addr": addr, "function": img.name(addr),
                     "float": bool(step[1]),
                     "unresolved_indirect": len(f.unresolved) if f else 0})
    return {"chain_depth": depth, "predicted_used": [lo, hi], "path": rows,
            "nodes": nodes, "recursion": recursion}


def analyse(img, spec, excluded, nominal_only, float_cut, failure_cuts,
            conditional_cuts, model, manual_edges):
    name, stack, free = parse_root(spec)
    addrs = img.lookup(name)
    if not addrs:
        die(f"no function named {name!r} (try --list-roots)")
    if len(addrs) > 1:
        die(f"{name!r} is ambiguous: " + ", ".join(img.name(a) for a in addrs))
    root = addrs[0]

    # Conditional edge cuts are decided per task: an edge that is feasible only
    # when some guard function is reachable is cut from both bounds for any root
    # that cannot reach a guard, and kept for any root that can.
    base_nodes = reachable(img, root, excluded)
    hard_cut, applied = set(), []
    for pairs, guards, desc in conditional_cuts:
        if base_nodes & guards:
            applied.append(("kept", desc))
        else:
            hard_cut |= pairs
            applied.append(("cut", desc))
    hard_cut = frozenset(hard_cut)
    soft_cut = hard_cut | failure_cuts

    # Float formatting is attributed per call site: the walk carries a "a float
    # formatter is already on this path" bit, and _dtoa_r may only be entered once
    # it is set. A deep chain whose own format strings hold no float is therefore
    # not charged for float formatting. float_free forces the bit to stay clear.
    formatters = frozenset(img.float_formats)
    worst = summarise(img, root, excluded, model, hard_cut, float_cut, formatters)
    nominal = summarise(img, root, excluded | set(nominal_only), model, soft_cut,
                        float_cut, formatters)
    float_free = summarise(img, root, excluded | set(nominal_only), model, soft_cut,
                           float_cut, frozenset())

    # The first failure/once function on the worst path is what carries the
    # worst case above the nominal bound - the actionable part of the report.
    via, prev = None, None
    for row in worst["path"]:
        if row["addr"] in nominal_only:
            via = {"kind": nominal_only[row["addr"]][0], "function": row["function"]}
            break
        if (prev, row["addr"]) in failure_cuts:
            via = {"kind": "failure edge", "function": row["function"]}
            break
        prev = row["addr"]

    nodes = worst["nodes"]
    unresolved, resolved = [], {"literal": 0, "vtable": 0}
    for a in nodes:
        f = img.funcs.get(a)
        if not f:
            continue
        unresolved += [(f.name, site, why) for site, why in f.unresolved]
        for _, how, _ in f.resolved:
            resolved["vtable" if how.startswith("vtable") else "literal"] += 1

    def reached(cut):
        return sorted({img.name(t) for n in nodes if img.funcs.get(n)
                       for t in img.funcs[n].edges if t in cut})

    result = {
        "task": img.name(root),
        "model": {"coprocessor": model["coprocessor"], "wrappers": model["wrappers"],
                  "align": list(model["align"]), "interrupt_frame": model["irq"]},
        "worst": {k: worst[k] for k in ("chain_depth", "predicted_used", "path")},
        "nominal": {k: nominal[k] for k in ("chain_depth", "predicted_used", "path")},
        "float_free": {k: float_free[k] for k in ("chain_depth", "predicted_used", "path")},
        "worst_exceeds_nominal_via": via,
        "reachable_functions": len(nodes),
        # Functions on the reachable set whose own `entry` could not be decoded, so their
        # frame counts as 0. This UNDER-states the bound, which is the dangerous direction,
        # so it is reported rather than left silent. Normally zero; a non-zero count means
        # objdump failed to disassemble part of the image.
        "frameless_functions": sorted(
            img.name(a) for a in nodes
            if a in img.funcs and img.funcs[a].frame is None),
        # Functions whose disassembly contains a branch to an address objdump never
        # decoded as an instruction - proof it lost instruction sync, so the call edges
        # it reported for them may be fiction. Unlike frameless_functions this OVER-states
        # (a phantom edge only ever adds depth), but it is what made NTPTask read 3248 B
        # instead of 1984 B, so any such function ON THE WORST PATH is called out below.
        "desynced_functions": sorted(
            {img.name(a) for a in nodes if a in img.funcs and img.funcs[a].desync}),
        # The actionable unit: an edge ON THE WORST PATH whose call site objdump printed
        # inside a mis-decoded span. These are the candidates for being pure fiction, and
        # checking whether each is semantically possible is a one-minute job. 13.6% of
        # functions contain a desynced span, so reporting at function granularity is noise -
        # only edges that actually carry the reported bound are listed.
        "fiction_edges_on_worst_path": [
            {"from": img.name(a), "to": img.name(b)}
            for a, b in zip([r["addr"] for r in worst["path"]],
                            [r["addr"] for r in worst["path"]][1:])
            if a in img.funcs and b in img.funcs[a].fiction_edges],
        "resolved_indirect": resolved,
        "unresolved_indirect": [{"function": fn, "site": f"{s:#010x}", "reason": r}
                                for fn, s, r in unresolved],
        "recursion": [{"members": m, "bound": k} for m, k in worst["recursion"]],
        "excluded_reached": reached(excluded),
        "nominal_cuts_reached": reached(set(nominal_only)),
        "manual_edges": manual_edges,
        "conditional_cuts": [{"decision": d, "edge": e} for d, e in applied],
        "float_formats_reachable": sorted(
            ({"function": img.name(a), "example": sorted(s)[0]}
             for a, s in img.float_formats.items() if a in nodes),
            key=lambda d: d["function"]),
    }
    if stack is not None:
        # free is None for the NAME:STACK form - a build that has never run, so there
        # is no high-water mark to check the bound against. Everything except the
        # measured-peak comparison still applies.
        observed = None if free is None else stack - free
        w_hi = worst["predicted_used"][1]
        n_hi = nominal["predicted_used"][1]
        f_hi = float_free["predicted_used"][1]
        if observed is not None and observed > n_hi:
            verdict = ("BOUND VIOLATED - observed exceeds the nominal bound: a failure/once "
                       "cut removed a path that actually ran, or a call edge is missing")
        elif w_hi <= stack:
            verdict = "fits, including the worst case"
        elif n_hi <= stack:
            verdict = "fits in nominal operation; worst case exceeds the stack only via failure/once code"
        elif f_hi <= stack:
            verdict = ("exceeds the stack only if a float is formatted on the deepest path "
                       "(_dtoa_r) - check this task's %f/%e/%g log calls")
        else:
            verdict = "STATIC BOUND EXCEEDS STACK even without float formatting - inspect the nominal path"
        result.update(stack_size=stack,
                      headroom_worst=stack - w_hi, headroom_nominal=stack - n_hi,
                      headroom_float_free=stack - f_hi, verdict=verdict)
        if observed is not None:
            result.update(observed_used=observed, bound_holds=observed <= n_hi,
                          observed_implies_float=observed > f_hi)
    return result


def print_report(r, show_path, show_unresolved):
    print(f"\n=== {r['task']} ===")
    m = r["model"]
    wsum = sum(fr for _, fr in m["wrappers"])
    wr = " + ".join(f"{n.split('::')[-1].split('(')[0]} {fr}" for n, fr in m["wrappers"])
    fixed = m["coprocessor"] + wsum
    a0, a1, irq = m["align"][0], m["align"][1], m["interrupt_frame"]
    print(f"  on top of the call chain: coprocessor {m['coprocessor']} + wrappers {wsum}"
          f" ({wr}) + alignment {a0}-{a1} + interrupt frame {irq} (_xt_lowint1)"
          f" = {fixed + a0 + irq}-{fixed + a1 + irq} B")
    for label, key in (("worst case, all paths", "worst"), ("nominal, failure/once cut", "nominal"),
                       ("nominal, no float formatting", "float_free")):
        b = r[key]
        lo, hi = b["predicted_used"]
        head = f"   headroom {r['stack_size'] - hi:+6d} B" if "stack_size" in r else ""
        print(f"  {label:27s} chain {b['chain_depth']:5d} B  ->  used {lo}-{hi} B{head}")
    via = r["worst_exceeds_nominal_via"]
    if via and r["worst"]["chain_depth"] != r["nominal"]["chain_depth"]:
        print(f"  worst case runs through {via['kind']} code: {via['function'][:80]}")
    if "observed_used" in r:
        print(f"  observed peak used          {r['observed_used']} B of {r['stack_size']}")
    if "verdict" in r:
        flag = "** " if r["verdict"].startswith(("BOUND", "STATIC")) else ""
        print(f"  verdict: {flag}{r['verdict']}")
        if "observed_used" not in r:
            print("  (static only - this build has no measured peak, so the bound is "
                  "unchecked against hardware)")
        if r.get("observed_implies_float") and r["bound_holds"]:
            print("  note: the observed peak is above the no-float bound, so this task has "
                  "formatted a float on a deep path")
    ri = r["resolved_indirect"]
    if r.get("frameless_functions"):
        fl = r["frameless_functions"]
        print(f"  ** {len(fl)} reachable function(s) have NO decodable `entry`, so their "
              f"frame counts as 0 - the bound is UNDER-stated. First: {fl[0][:70]}")
    for fe in r.get("fiction_edges_on_worst_path", ()):
        print(f"  ** PHANTOM-EDGE SUSPECT carrying this bound: "
              f"{fe['from'].split('(')[0][:44]} -> {fe['to'].split('(')[0][:44]}  "
              f"(objdump lost instruction sync at the call site, so the edge may not exist; "
              f"the bound would then be OVER-stated). Verify it is semantically possible.")
    print(f"  {r['reachable_functions']} reachable functions; indirect calls resolved: "
          f"{ri['literal']} literal-pool, {ri['vtable']} vtable; "
          f"{len(r['unresolved_indirect'])} unresolved")
    ff = r["float_formats_reachable"]
    if ff:
        print(f"  {len(ff)} reachable function(s) format a float, e.g. "
              f"{ff[0]['function'].split('(')[0][:46]}: \"{ff[0]['example'][:44]}\"")
    else:
        print("  no reachable function formats a float")
    for decision, edge in ((c["decision"], c["edge"]) for c in r["conditional_cuts"]):
        print(f"  conditional {decision}: {edge}")
    for rec in r["recursion"]:
        names = ", ".join(rec["members"][:3]) + (" ..." if len(rec["members"]) > 3 else "")
        print(f"  cycle ({rec['bound']}, {len(rec['members'])} fns, counted once): {names[:110]}")
    if show_path:
        for key in ("worst", "nominal", "float_free"):
            print(f"  {key} path (frame / cumulative):")
            for row in r[key]["path"]:
                ind = f"   [{row['unresolved_indirect']} unresolved indirect]" \
                    if row.get("unresolved_indirect") else ""
                print(f"    {row['frame']:5d} {row['cumulative']:6d}  {row['function'][:90]}{ind}")
    if show_unresolved and r["unresolved_indirect"]:
        print("  unresolved indirect call sites:")
        for u in r["unresolved_indirect"][:show_unresolved]:
            print(f"    {u['site']}  {u['function'][:58]}: {u['reason']}")


def main():
    ap = argparse.ArgumentParser(
        description="Static worst-case stack bound for ESP32 Xtensa FreeRTOS tasks.",
        epilog="ROOT may be NAME or NAME:STACK_SIZE:FREE_BYTES (FREE from uxTaskGetStackHighWaterMark) "
               "to check the bound against a measured peak.")
    ap.add_argument("roots", nargs="*", help="task entry function(s)")
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--stale-excludes-ok", action="store_true",
        help="downgrade stale stack_walker.excludes entries from a fatal error to a "
             "listed warning. For validating a DIFFERENT build environment: the "
             "excludes were verified against esp32dev_usb_debug_selective, and an "
             "entry can legitimately not apply to another log mode (a cut whose "
             "LOG_* call compiles away has no edge to cut). Never use this on the "
             "primary env - there the hard failure is what catches rot.")
    ap.add_argument("--rom-elf", default=os.path.expanduser(
        "~/.platformio/packages/tool-esp-rom-elfs/esp32_rev0_rom.elf"),
        help="ROM image for calls into mask ROM ('' to disable)")
    ap.add_argument("--exclude", action="append", default=[], metavar="FUNC",
                    help="cut the graph at this function (repeatable)")
    ap.add_argument("--no-default-excludes", action="store_true",
                    help="also walk into abort/assert/panic paths")
    ap.add_argument("--no-project-excludes", action="store_true",
                    help="ignore tools/stack_walker.excludes (configuration-unreachable code)")
    ap.add_argument("--edge", action="append", default=[], metavar="CALLER=CALLEE",
                    help="add a call edge the analysis cannot see (repeatable)")
    ap.add_argument("--no-task-manager-wrapper", action="store_true",
                    help="task was created with raw xTaskCreate, not TaskManager")
    ap.add_argument("--list-roots", action="store_true",
                    help="list candidate task entry functions")
    ap.add_argument("--path", action="store_true", help="print the deepest path")
    ap.add_argument("--unresolved", type=int, default=0, metavar="N",
                    help="print up to N unresolved indirect call sites")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.elf):
        die(f"{args.elf} not found - build first, or pass --elf")
    rom = args.rom_elf if args.rom_elf and os.path.exists(args.rom_elf) else None
    img = Image(args.elf, rom)

    if args.list_roots:
        for n in sorted(n for n in img.by_name if n.endswith("(void*)") and not n.endswith("@rom")):
            print(n)
        return
    if not args.roots:
        ap.error("give at least one task entry function, or --list-roots")

    excluded = set()
    if not args.no_default_excludes:
        for n, addrs in img.by_name.items():
            if base_name(n.replace("@rom", "")) in NORETURN:
                excluded.update(addrs)
    # Project exclusions (tools/stack_walker.excludes). Every entry carries a
    # mandatory reason. Kinds:
    #   FUNC                never runs in this firmware   -> cut from both bounds
    #   cut: CALLER -> CALLEE  that call never happens    -> cut from both bounds
    #   failure: FUNC       only runs after a failure     -> cut from nominal only
    #   once: FUNC          one-time initialisation       -> cut from nominal only
    project_excludes = []
    nominal_only = {}             # addr -> (kind, name)
    failure_cuts = set()          # (caller, callee) edges cut from the nominal bound only
    conditional_cuts = []         # (edge pairs, guard addrs, description), decided per task

    stale_entries = []            # (lineno, spec, why) tolerated under --stale-excludes-ok

    def stale(lineno, spec, why):
        """A .excludes entry that does not match this image.

        Fatal by default: a stale entry silently under-reports stack, which is the
        one failure mode this file must not have. Tolerated only when the caller
        has said they are deliberately pointing the tool at another build
        environment, and even then every skipped entry is listed in the report."""
        if args.stale_excludes_ok:
            stale_entries.append((lineno, spec, why))
            return None
        die(f"{xfile}:{lineno}: {spec!r} {why}")

    def edge_pairs(spec, lineno):
        caller, sep, callee = spec.partition("->")
        if not sep:
            die(f"{xfile}:{lineno}: expected CALLER -> CALLEE in {spec!r}")
        ca, cb = img.lookup(caller.strip()), set(img.lookup(callee.strip()))
        if not ca or not cb:
            return stale(lineno, spec, "names a function this image does not have")
        pairs = {(a, b) for a in ca for b in img.funcs[a].edges if b in cb}
        if not pairs:
            return stale(lineno, spec, "matches no existing call edge - stale entry?")
        return pairs
    xfile = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stack_walker.excludes")
    if not args.no_project_excludes and os.path.exists(xfile):
        with open(xfile) as f:
            for lineno, line in enumerate(f, 1):
                body, _, reason = line.partition("#")
                body, reason = body.strip(), reason.strip()
                if not body:
                    continue
                if not reason:
                    die(f"{xfile}:{lineno}: {body!r} has no reason")
                kind, _, rest = body.partition(":")
                if rest and kind.strip() in ("cut", "edge", "failure", "once",
                                             "failure-cut", "cut-unless-reachable"):
                    kind, body = kind.strip(), rest.strip()
                else:
                    kind = "unreachable"
                if kind == "failure-cut":
                    # One call that happens only on a failure (e.g. a print right
                    # before abort) inside a function that otherwise runs normally.
                    pairs = edge_pairs(body, lineno)
                    if pairs is None:
                        continue  # stale under --stale-excludes-ok
                    failure_cuts |= pairs
                    project_excludes.append((kind, body, reason))
                    continue
                if kind == "cut-unless-reachable":
                    # "CALLER -> CALLEE | GUARD, GUARD": the call is feasible only
                    # when a guard function can run, so it is cut per task.
                    spec, sep, guard_list = body.partition("|")
                    if not sep:
                        die(f"{xfile}:{lineno}: expected CALLER -> CALLEE | GUARD[, GUARD]")
                    guards = set()
                    for g in guard_list.split(","):
                        hits = img.lookup(g.strip())
                        if not hits:
                            die(f"{xfile}:{lineno}: unknown guard function {g.strip()!r}")
                        guards.update(hits)
                    pairs = edge_pairs(spec, lineno)
                    if pairs is None:
                        continue  # stale under --stale-excludes-ok
                    conditional_cuts.append((pairs, guards, body))
                    project_excludes.append((kind, body, reason))
                    continue
                if kind == "edge":
                    # A call the analysis cannot see (e.g. a function pointer passed
                    # in by the caller). Added to both bounds.
                    caller, sep, callee = body.partition("->")
                    ca, cb = img.lookup(caller.strip()), img.lookup(callee.strip())
                    if not sep:
                        die(f"{xfile}:{lineno}: edge needs one CALLER -> one CALLEE: {body!r}")
                    # NOT routed through stale(): an `edge:` is the one kind that ADDS a
                    # call edge, so skipping it makes the bound SMALLER, not larger. It is
                    # fatal even under --stale-excludes-ok. Demonstrated cost of getting
                    # this wrong: -1760 B and a verdict flip from "STATIC BOUND EXCEEDS
                    # STACK" to "fits" on RYN4Proc, while the tool printed a note claiming
                    # skips are always conservative. An ambiguous endpoint (a .constprop/
                    # .isra clone in another build) is fatal for the same reason - it is
                    # never "a cut whose LOG_* call compiled away".
                    if len(ca) != 1 or len(cb) != 1:
                        die(f"{xfile}:{lineno}: edge needs one CALLER -> one CALLEE "
                            f"(found {len(ca)} and {len(cb)}): {body!r}. `edge:` is fatal "
                            f"even under --stale-excludes-ok, because skipping an added "
                            f"edge REDUCES the bound.")
                    img.funcs[ca[0]].edges.add(cb[0])
                    project_excludes.append((kind, body, reason))
                    continue
                if kind == "cut":
                    caller, sep, callee = body.partition("->")
                    if not sep:
                        die(f"{xfile}:{lineno}: cut needs CALLER -> CALLEE")
                    ca, cb = img.lookup(caller.strip()), set(img.lookup(callee.strip()))
                    if not ca or not cb:
                        if stale(lineno, body, "names a function this image does not have") is None:
                            continue
                    removed = 0
                    for a in ca:
                        before = len(img.funcs[a].edges)
                        img.funcs[a].edges -= cb
                        removed += before - len(img.funcs[a].edges)
                    if not removed:
                        if stale(lineno, body,
                                 "matches no existing call edge - stale entry?") is None:
                            continue
                else:
                    addrs = img.lookup(body)
                    if not addrs:
                        if stale(lineno, body,
                                 "names a function this image does not have") is None:
                            continue
                    if kind == "unreachable":
                        excluded.update(addrs)
                    else:
                        for a in addrs:
                            nominal_only[a] = (kind, body)
                project_excludes.append((kind, body, reason))
    for x in args.exclude:
        addrs = img.lookup(x)
        if not addrs:
            die(f"--exclude {x!r}: no such function")
        excluded.update(addrs)

    manual = []
    for spec in args.edge:
        if "=" not in spec:
            die(f"--edge {spec!r}: expected CALLER=CALLEE")
        a, b = spec.split("=", 1)
        ca, cb = img.lookup(a), img.lookup(b)
        if len(ca) != 1 or len(cb) != 1:
            die(f"--edge {spec!r}: caller or callee missing or ambiguous")
        img.funcs[ca[0]].edges.add(cb[0])
        manual.append(spec)

    model = img.model(task_manager_wrapper=not args.no_task_manager_wrapper)
    float_cut = {a for n, addrs in img.by_name.items()
                 if base_name(n.replace("@rom", "")) == "_dtoa_r" for a in addrs}
    results = [analyse(img, spec, excluded, nominal_only, float_cut, frozenset(failure_cuts),
                       conditional_cuts, model, manual)
               for spec in args.roots]

    if args.json:
        json.dump({"model_notes": model["notes"],
                   "project_excludes": [{"kind": k, "entry": b, "reason": r}
                                        for k, b, r in project_excludes],
                   "skipped_excludes": [{"line": n, "entry": e, "why": w}
                                        for n, e, w in stale_entries],
                   "command_line_excludes": args.exclude,
                   "tasks": results}, sys.stdout, indent=2)
        print()
    else:
        for note in model["notes"]:
            print(f"note: {note}")
        for kind, body, reason in project_excludes:
            print(f"project {kind}: {body}  -- {reason}")
        for lineno, spec, why in stale_entries:
            print(f"SKIPPED stack_walker.excludes:{lineno}: {spec}  -- {why}")
        if stale_entries:
            print(f"NOTE: {len(stale_entries)} exclusion(s) skipped via --stale-excludes-ok. "
                  "Skipping any of these kinds - FUNC/unreachable, cut, cut-unless-reachable, "
                  "failure, failure-cut, once - retains an edge or retains a function in the "
                  "nominal bound, so it makes the bound MORE pessimistic and a 'fits' verdict "
                  "still holds. `edge:` is NOT in that set and is never skipped: it ADDS an "
                  "edge, so skipping one would SHRINK the bound, and it stays fatal.")
        for name in args.exclude:
            print(f"command-line exclude: {name}")
        for r in results:
            print_report(r, args.path, args.unresolved)

    # Exit status is usable from CI: 2 = a measured peak broke the nominal bound
    # (the analysis is missing something), 3 = the static bound exceeds the stack
    # even with float formatting removed.
    if any(r.get("bound_holds") is False for r in results):
        sys.exit(2)
    if any(r.get("verdict", "").startswith("STATIC BOUND") for r in results):
        sys.exit(3)


if __name__ == "__main__":
    main()
