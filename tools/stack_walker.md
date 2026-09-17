# stack_walker.py - static stack bounds for FreeRTOS tasks

Answers "how deep can this task's stack go?" from the linked firmware image,
and checks the answer against a peak measured on the device.

```bash
pio run -e esp32dev_usb_debug_selective          # the tool reads the built ELF
tools/stack_walker.py BoilerTempControlTask:3584:1044
tools/stack_walker.py PumpControlModule::WaterPumpTask:2560:928 --path
tools/stack_walker.py --list-roots
tools/stack_walker.py --no-task-manager-wrapper Logger::subscriberTaskFunc:5120:4308
```

`NAME:STACK:FREE` takes the configured stack size and the free bytes reported by
`boiler/cmd/diagnostics` payload `tasks` (the "Stack Free" column, already in
bytes). Without `:STACK:FREE` only the bounds are printed. `--json` gives
machine-readable output. Needs no Python packages beyond the standard library.

## What a measured peak actually contains

A high-water mark is not just the call chain:

```
measured used = task prefix + call-chain depth + one interrupt frame
```

| part | size here | read from |
|---|---|---|
| coprocessor save area | 96 B | `pxPortInitialiseStack` (`addi aN, a2, -96`) |
| task wrappers | 32 + 32 B | `vPortTaskWrapper`, `TaskManager::safeTaskWrapper` prologues |
| stack-top alignment | 4-16 B | heap 4-byte alignment, top masked to 16 |
| interrupt frame | 192 B | `_xt_lowint1` (`addmi a1,a1,-256; addi a1,a1,64`) |

The interrupt frame lands on the **interrupted task's** stack before the switch
to the ISR stack, so `CONFIG_FREERTOS_ISR_STACKSIZE` does not cover it. It also
makes peaks slightly stochastic: two identical tasks can differ by where an
interrupt happened to land. Comparing a bare frame sum against a high-water mark
is meaningless - an early version of this analysis did exactly that and
"matched" by coincidence.

## The three bounds

| bound | what is included |
|---|---|
| **worst case** | every reachable path, including failure handling and one-time init |
| **nominal** | worst case minus `failure:`/`once:` code from `stack_walker.excludes` |
| **no-float** | nominal minus float formatting (`_dtoa_r` and below) |

A call graph cannot see format strings, so every log call is charged the float
path: `_svfprintf_r` (800 B) -> `_dtoa_r` -> first-use `_Balloc` -> `calloc` ->
heap. That is the *real* peak for tasks that log floats and phantom for tasks
that do not, so it is reported as a condition rather than silently kept or cut.
If a measured peak exceeds the no-float bound, the task **has** formatted a
float, and the report says so.

Verdicts, in order: `BOUND VIOLATED` (measured > nominal - the analysis is
missing something) · `fits, including the worst case` · `fits in nominal
operation` (worst case exceeds only via failure/once code) · `exceeds the stack
only if a float is formatted on the deepest path` · `STATIC BOUND EXCEEDS STACK
even without float formatting`.

Exit status: `0` ok, `1` usage/stale exclusion, `2` a measured peak broke the
nominal bound, `3` a static bound exceeds the stack without float formatting.

## How calls are resolved

A naive walker over `objdump -d` misses most of the graph. This one resolves:

- **direct** `call0/4/8/12 <target>`, plus `j` tail calls into another function
- **literal pool** - `l32r aN, LIT` ... `callx8 aN`, the `-mlongcalls` pattern;
  the word at `LIT` is read from the ELF load segments. Most "indirect" calls in
  this firmware are really this (540 resolved on the BoilerTempCtrl subgraph, with
  62 genuinely unresolved).
- **vtable** - `call8 C::getInstance()` -> load vptr -> load slot -> `callx8`,
  through the `vtable for C` symbol. This is how `custom_log_write` reaches
  `Logger::logV`, which a naive walker never follows.
- **mask ROM** - calls into ROM resolve against `esp32_rev0_rom.elf`.

Targets are resolved only after the whole listing is parsed: a callee that sits
later in address order than its caller is not yet known while the caller is
being read, which silently dropped every forward edge in an earlier version.

Remaining unresolved indirect sites (function pointers, stream callbacks) are
counted in every report. They are where missing depth would hide.

## Maintaining `stack_walker.excludes`

Every entry needs a reason after `#` that says **why and how to re-check it**,
and every fact must be verified in the disassembly - not inferred from a
function name. A stale entry fails loudly (`matches no existing call edge`).

| kind | meaning | applies to |
|---|---|---|
| `FUNC` | never runs in this firmware | both bounds |
| `cut: A -> B` | that call never happens | both bounds |
| `cut-unless-reachable: A -> B \| G` | call is feasible only if `G` is reachable; decided per task | both bounds |
| `edge: A -> B` | a real call the tool cannot see | both bounds |
| `failure: FUNC` | runs only after a failure | nominal only |
| `failure-cut: A -> B` | one call that happens only on failure | nominal only |
| `once: FUNC` | one-time initialisation | nominal only |

`failure:`/`once:` never shrink the worst case, so "could error handling
overflow this task?" is always still answered. Re-run the validation below after
changing an entry: if any measured peak exceeds its nominal bound, an entry is
wrong.

## Validation (image 8ee1446)

Measured peaks from the post-flash task dump at **64 s uptime**. A high-water
mark only ever falls, so an early reading is a weaker test than a long-uptime one;
the pump and BoilerTempCtrl marks were independently identical at 11 h.

| task | measured | no-float | nominal | worst | stack | verdict |
|---|---|---|---|---|---|---|
| ANDRTF3 | 1660 | 2272 | 2576 | 2992 | 3584 | fits |
| BoilerTempCtrl | 2540 | 2928 | 3232 | 3312 | 3584 | fits |
| BurnerControl | 1768 | 3200 | 3504 | 3504 | 4096 | fits |
| ControlTask | 1632 | 2208 | 2512 | 2816 | 3584 | fits |
| HeatingControl | 1604 | 3280 | 3584 | 3584 | 3584 | fits (0 B worst-case headroom) |
| HeatingPump | 1632 | 2352 | 2656 | 2928 | 2560 | float-conditional |
| WaterPump | 1632 | 2352 | 2656 | 2928 | 2560 | float-conditional |
| MB8ART | 1796 | 2256 | 2560 | 3040 | 3584 | fits |
| MB8ARTProc | 1488 | 2096 | 2400 | 2400 | 3072 | fits |
| Monitoring | 1732 | 3232 | 3536 | 3536 | 3584 | fits |
| MQTTTask | 2436 | 3008 | 3312 | 3360 | 3584 | fits |
| NTPTask | 2784 | 2512 | 2816 | 2816 | 4096 | fits - **formatted a float** |
| OTATask | 1800 | 3008 | 3312 | 3312 | 3072 | float-conditional |
| PersistentStorage | 1860 | 4272 | 4576 | 4576 | 5120 | fits |
| RelayControl | 1696 | 2384 | 2688 | 2992 | 4096 | fits |
| RYN4Proc | 1688 | 2464 | 2768 | 3072 | 2560 | float-conditional |
| TimerSched | 1788 | 2832 | 3136 | 3136 | 3072 | float-conditional |
| WheaterControl | 1684 | 3520 | 3824 | 3824 | 3584 | float-conditional |
| LogSub (raw `xTaskCreate`) | 812 | 976 | 976 | 976 | 5120 | fits |

All 19 measured peaks are within their nominal bound. NTPTask is the sharpest
check both ways: nominal is only 32 B above the measurement, and the measurement
exceeds the no-float bound - which `NTPClient.cpp` confirms with
`"Average sync time: %.1fms"`.

"Float-conditional" means the task fits unless a float is formatted on its
deepest path. For the pump tasks that is almost certainly not the case - the
pump code formats no floats. **WheaterControl is the one worth checking**: its
nominal bound is 240 B over its stack, water heating works in temperatures, and
the `LOG_MODE_DEBUG_FULL` stack comment for this task cites float formatting (the
active `DEBUG_SELECTIVE` comment does not). Not yet investigated.

## Limits

- Context-insensitive: branches that cannot co-occur can be combined. The
  excludes file removes the verified cases; others make the bound pessimistic.
- Register tracking is linear, with no control-flow merge.
- Each recursion cycle is counted once; true recursion depth is not bounded.
- Unresolved indirect calls can hide depth. A measured peak above the nominal
  bound (exit 2) is the signal that something was missed.
