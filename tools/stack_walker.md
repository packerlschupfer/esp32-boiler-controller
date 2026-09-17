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

Float formatting is expensive - `_svfprintf_r` (800 B) -> `_dtoa_r` -> first-use
`_Balloc` -> `calloc` -> heap - so charging it to a task that never formats a
float produces false alarms. The tool decides this from the binary: it collects
every format string reached through the literal pool, and charges the float path
only to tasks whose reachable set contains one with a `%f`/`%e`/`%g` conversion.
Tasks with none get the no-float bound as their nominal bound, and the report
names the function and string for those that do.

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

| task | measured | no-float | nominal | worst | stack | floats? | verdict |
|---|---|---|---|---|---|---|---|
| ANDRTF3Task | 1660 | 2272 | 2272 | 2688 | 3584 | no | fits |
| BoilerTempControlTask | 2540 | 2928 | 3232 | 3312 | 3584 | yes | fits |
| BurnerControlTask | 1768 | 3200 | 3200 | 3200 | 4096 | no | fits |
| ControlTask | 1632 | 2208 | 2208 | 2512 | 3584 | no | fits |
| HeatingControlTask | 1604 | 3280 | 3584 | 3584 | 3584 | yes | fits |
| HeatingPumpTask | 1632 | 2352 | 2352 | 2624 | 2560 | no | fits nominally |
| WaterPumpTask | 1632 | 2352 | 2352 | 2624 | 2560 | no | fits nominally |
| MB8ARTTask | 1796 | 2256 | 2256 | 2736 | 3584 | no | fits |
| MB8ARTProcessingTask | 1488 | 2096 | 2096 | 2096 | 3072 | no | fits |
| MonitoringTask | 1732 | 3232 | 3536 | 3536 | 3584 | yes | fits |
| MQTTTask | 2436 | 3008 | 3312 | 3360 | 3584 | yes | fits |
| NTPTask | 2784 | 2512 | 2816 | 2816 | 4096 | yes | fits |
| OTATask | 1800 | 3008 | 3008 | 3008 | 3072 | no | fits |
| PersistentStorageTask | 1860 | 4272 | 4272 | 4272 | 5120 | no | fits |
| RelayControlTask | 1696 | 2384 | 2384 | 2688 | 4096 | no | fits |
| RYN4ProcessingTask | 1688 | 2464 | 2464 | 2768 | 2560 | no | fits nominally |
| TimerSchedulerTask | 1788 | 2832 | 2832 | 2832 | 3072 | no | fits |
| WheaterControlTask | 1684 | 3520 | 3824 | 3824 | 3584 | yes | float-conditional |
| LogSub (raw xTaskCreate) | 812 | 976 | 976 | 976 | 5120 | no | fits |

All 19 measured peaks are within their nominal bound. NTPTask is the sharpest
check both ways: nominal is only 32 B above the measurement, and the measurement
exceeds the no-float bound - which `NTPClient.cpp` confirms with
`"Average sync time: %.1fms"`.

### What the float column resolved

Float formatting is charged only to tasks that can reach a `%f`-style format
string, detected from the literal pool. That decided five of the six tasks that
were previously reported as "float-conditional": HeatingPump, WaterPump, OTATask,
RYN4Proc and TimerSched reference **no** float format string anywhere in their
reachable set, so they fit.

**WheaterControl** is the only remaining one. It reaches exactly one float
format - `calculateBoilerTarget()` logging `"Invalid charge delta: %.2f, using
default"`, a validation-failure message - so the tool conservatively charges the
float path to its deepest chain (the `publishFallbackModeChange` ->
`MQTTTask::publish` chain, whose own format strings contain no float) and reports
3824 B. Attributing the float cost to the call site that can actually produce it
gives a much smaller number:

```
WheaterControlTask            48
processWaterHeatingState     384
calculateBoilerTarget + tail 1968   (measured by rooting the tool there)
                            -----
chain                       2400  + prefix/interrupt 356-368  =  2756-2768 B
```

So WheaterControl's float-capable chain is ~800 B clear of its 3584 B stack, and
its real bound is `max(no-float 3520, float-feasible 2768)` = 3520 B. It fits.
Per-call-site float attribution is not implemented - it needs the float sink to
be reachable only after a float-formatting function on the same path (a two-state
product graph). Until then this task reads as float-conditional.

### Worst case above the stack: pumps and RYN4Proc

HeatingPump, WaterPump (2624 B) and RYN4Proc (2768 B) exceed their 2560 B stacks
in the **worst case** only, and all three are carried by the same `once:` entry -
`SharedResourceManager::SharedResourceManager()`, reached through `getInstance()`.
That constructor runs on the boot task: `main.cpp:249` constructs the singleton
explicitly, and `InitStage::SHARED_RESOURCES` (`SystemInitializer.cpp:113`)
completes before `initializeTasks()` at `:186`. These tasks therefore never
execute it, which is what `once:` encodes - the nominal bound applies and they
fit.

**No task is at risk on this image**: every measured peak is within its nominal
bound, the three worst-case exceedances are boot-only construction, and
WheaterControl's float-capable chain is ~800 B clear of its stack.

## Limits

- Context-insensitive: branches that cannot co-occur can be combined. The
  excludes file removes the verified cases; others make the bound pessimistic.
- Register tracking is linear, with no control-flow merge.
- Each recursion cycle is counted once; true recursion depth is not bounded.
- Unresolved indirect calls can hide depth. A measured peak above the nominal
  bound (exit 2) is the signal that something was missed.
- Float formatting is attributed per **task**, not per call site: if any reachable
  function formats a float, the cost is charged to the task's deepest chain even
  when that chain's own format strings contain none. This is what leaves
  WheaterControl reading float-conditional. Fixing it needs the float sink to be
  reachable only after a float-formatting function on the same path - a two-state
  product graph over (node, seen-a-float-formatter).
- Float format strings are found through the literal pool, so a format string
  built at run time or passed in as a parameter is not seen.
