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

`NAME`, `NAME:STACK` or `NAME:STACK:FREE`. The two-field form asks "does the static
bound fit this configured stack?" with no measured peak - the only form available for a
build that has never run, such as another log mode; passing a fake free of 0 instead would
read as "measured used == stack" and trip the bound-violated check. `--stale-excludes-ok`
tolerates exclusions that do not apply to a foreign ELF (see below).

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

Float formatting is expensive - `_svfprintf_r` (800 B) -> `_dtoa_r` -> first-use
`_Balloc` -> `calloc` -> heap - so charging it to a chain that cannot produce a
float produces false alarms. The tool decides this from the binary: every
literal-pool string is scanned for a `%f`/`%e`/`%g` conversion, and the search
runs over a product graph of (function, *a float formatter is already on this
path*). `_dtoa_r` may only be entered once that bit is set, so the cost lands on
the chains that can actually format a float and nowhere else. The report names
the function and string for tasks that reach one.

If a measured peak exceeds the no-float bound, the task **has** formatted a
float, and the report says so - an independent cross-check on the model.

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
function name. A stale entry fails loudly (`matches no existing call edge`). `--stale-excludes-ok`
downgrades that to a listed warning for cross-environment runs, because an entry verified
against `esp32dev_usb_debug_selective` can legitimately not apply to another log mode - a
`cut:` whose `LOG_*` call compiles away has no edge to cut. **`edge:` is exempt and stays
fatal**: it is the one kind that *adds* depth, so skipping it would make the bound
*smaller*. Getting that wrong was measured at -1760 B and a verdict flip from `STATIC
BOUND EXCEEDS STACK` to `fits`.

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

| task | measured | no-float | nominal | worst | stack | floats? | verdict |
|---|---|---|---|---|---|---|---|
| ANDRTF3Task | 1660 | 2272 | 2272 | 2688 | 3584 | no | fits |
| BoilerTempControlTask | 2540 | 2928 | 3232 | 3312 | 3584 | yes | fits |
| BurnerControlTask | 1768 | 3200 | 3200 | 3200 | 4096 | no | fits |
| ControlTask | 1632 | 2208 | 2208 | 2512 | 3584 | no | fits |
| HeatingControlTask | 1604 | 3280 | 3280 | 3280 | 3584 | yes | fits |
| HeatingPumpTask | 1632 | 2320 | 2320 | 2544 | 2560 | no | fits |
| WaterPumpTask | 1632 | 2320 | 2320 | 2544 | 2560 | no | fits |
| MB8ARTTask | 1796 | 2256 | 2256 | 2736 | 3584 | no | fits |
| MB8ARTProcessingTask | 1488 | 2096 | 2096 | 2096 | 3072 | no | fits |
| MonitoringTask | 1732 | 3232 | 3536 | 3536 | 3584 | yes | fits |
| MQTTTask | 2436 | 3008 | 3312 | 3360 | 3584 | yes | fits |
| NTPTask | 2784 | 2512 | 2816 | 2816 | 3072 | yes | fits |
| OTATask | 1800 | 3008 | 3008 | 3008 | 3072 | no | fits |
| PersistentStorageTask | 1860 | 4272 | 4272 | 4272 | 5120 | no | fits |
| RelayControlTask | 1696 | 2384 | 2384 | 2688 | 4096 | no | fits |
| RYN4ProcessingTask | 1688 | 2464 | 2464 | 2768 | 3072 | no | fits |
| TimerSchedulerTask | 1788 | 2832 | 2832 | 2832 | 3072 | no | fits |
| WheaterControlTask | 1684 | 3520 | 3520 | 3520 | 3584 | yes | fits |
| LogSub (raw xTaskCreate) | 812 | 976 | 976 | 976 | 5120 | no | fits |

All 19 measured peaks are within their nominal bound, and every task fits. NTPTask
is the sharpest check both ways: nominal is only 32 B above the measurement, and
the measurement exceeds the no-float bound - which `NTPClient.cpp` confirms with
`"Average sync time: %.1fms"`.

### What float attribution resolved

Float cost is charged per **call site**, not per task: the walk carries a
"a float-formatting function is already on this path" bit and may only enter
`_dtoa_r` once it is set. Six tasks were previously reported as float-conditional;
all six now fit.

- HeatingPump, WaterPump, OTATask, RYN4Proc, TimerSched reach **no** float format
  string at all.
- **WheaterControl** reaches exactly one - `calculateBoilerTarget()` logging
  `"Invalid charge delta: %.2f, using default"`, a validation-failure branch. That
  chain measures 48 + 384 + 1968 = 2400 B + prefix/interrupt = 2756-2768 B, which
  is *shallower* than its float-free deep chain (`publishFallbackModeChange` ->
  `MQTTTask::publish`, 3520 B). Charging the float cost to the deep chain gave
  3824 B; attributing it correctly gives 3520 B, and the task fits.
- **HeatingControlTask** improved the same way, 3584 -> 3280 B. It had been sitting
  at exactly zero bound headroom.

Tightest bound headroom now: Monitoring +48 B, OTATask +64 B, WheaterControl +64 B.
These are upper bounds - the measured peaks are 1732, 1800 and 1684 B - so none of
them justifies spending DRAM on a bigger stack today.

### The failsafe callback dispatch had to be added by hand

`CentralizedFailsafe::executeFailsafeActions()` invokes its registered subsystem
callbacks as `callback.second(level, reason)` - a call through a container of
callables, which no call-graph walk can follow. The tool reported it as
`[1 unresolved indirect]` and silently omitted the deepest thing the failsafe path
does. Two `edge:` entries in `stack_walker.excludes` now model it
(`defaultBurnerFailsafe`, chain 1872 B; `defaultPumpFailsafe`, 1728 B).

The bounds did not move - the callback branch reaches 224 + 1872 = 2096 B, exactly
matching the already-modelled `emergencyStop` branch - but that was luck, not design,
and the edges make a future change to those callbacks visible instead of invisible.
This is the concrete example of what the "unresolved indirect calls can hide depth"
limit below actually costs: ~1.9 kB, on the deepest path of the task that writes the
relays, found only by reading the source.

### Worst case above the stack: nobody, as of 2026-09-18

This section used to list HeatingPump, WaterPump and RYN4Proc. On the current image none of them exceeds its stack: RYN4Proc was raised to 3072 and both pumps now measure a 2544 B worst case against 2560 B. The mechanism is still worth recording, because it is what a future `once:`-carried exceedance will look like. The pumps (2544 B) come within 16 B of their 2560 B stacks
in the **worst case** only, and all three are carried by the same `once:` entry -
`SharedResourceManager::SharedResourceManager()`, reached through `getInstance()`.
That constructor runs on the boot task: `main.cpp:249` constructs the singleton
explicitly, and `InitStage::SHARED_RESOURCES` (`SystemInitializer.cpp:113`)
completes before `initializeTasks()` at `:186`. These tasks therefore never
execute it, which is what `once:` encodes - the nominal bound applies and they
fit.

**No task is at risk on this image**: every measured peak is within its nominal
bound, the remaining worst-case margin is boot-only construction, and
WheaterControl's float-capable chain is ~800 B clear of its stack.

## Limits

- Context-insensitive: branches that cannot co-occur can be combined. The
  excludes file removes the verified cases; others make the bound pessimistic.
- Register tracking is linear, with no control-flow merge.
- Each recursion cycle is counted once; true recursion depth is not bounded.
- Unresolved indirect calls can hide depth. A measured peak above the nominal
  bound (exit 2) is the signal that something was missed.
- Float format strings are found through the literal pool, so a format string
  built at run time or passed in as a parameter is not seen. The float bit is
  also set by *entering* a function that holds such a string, not by the call
  that consumes it, so a function holding both a float and a non-float format
  charges the float cost to either.
