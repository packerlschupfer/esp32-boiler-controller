# Architecture Diagrams

**Note**: These diagrams are checked against the code by hand and can lag behind it. Cross-check with the current documentation and the code before relying on details.

**Two portability traps, both found on 2026-11-04 and both invisible in gDiagram:**
1. Stock PlantUML silently CROPS at 4096 px - it exits 0 and writes a truncated image.
   gDiagram renders at any size and ignores `scale max 4000 width`, so a file can look
   complete here and be cut off everywhere else. Measure the PNG, do not trust the exit
   code. `CompleteSystemArchitecture2025` and `ParameterManagementFlow` carry the cap.
2. gDiagram accepts escaped quotes inside a quoted label (`"NVS\nnamespace \"boot\""`);
   stock PlantUML 1.2026.9 rejects the file outright. Avoid them.

Both engines are worth running before committing a diagram:
`gdiagram F.puml -e out.png --theme light` and `java -jar /usr/local/lib/plantuml.jar -tpng F.puml`.

## Diagrams

All sources are PlantUML files in `plantuml/`; each one includes the shared dark theme `plantuml/theme.puml`.

- `BootDiagnostics.puml` - Sequence: why the controller restarted - esp_reset_reason() captured at boot, published retained on connect
- `BurnerStateMachine.puml` - Burner state machine: 9 states, which of them are LIT (burnerLit), and their transitions
- `BurnerSystemController.puml` - BurnerSystemController class, burner relays R1-R3 and pump separation
- `CompleteSystemArchitecture2025.puml` - Deployment view: shared resources, Modbus, storage, network, MQTT topics
- `ControlRequestFlow.puml` - Sequence: heating/water requests, water priority, demand arming, burner relays
- `FrostProtection.puml` - Activity: the ONE frost latch (flow, room or tank set it; it drives space heating), the valid-AND-fresh trigger rule
- `HeatingControlLogic.puml` - Activity: HeatingControlTask, weather vs room mode, heating curve, burner request
- `HotWaterScheduler.puml` - Aspirational pre-heating scheduler (not implemented; TimerSchedulerTask exists)
- `ModbusBusOwnership.puml` - RS485 bus release: one bus three devices, the refusal reasons, and the DELAY watchdog that makes it safe
- `LibraryEcosystem.puml` - Custom libraries and their dependencies
- `ParameterManagementFlow.puml` - Component view: MQTT parameter topics, PersistentStorage, NVS
- `ParameterUpdateSequence.puml` - Sequence: parameter set, save, get/list
- `SafetyErrorHandling.puml` - Error sources, failsafe levels, emergency stop and release, error reporting
- `SharedResourceManager.puml` - Event groups, standard mutexes and queues
- `StorageArchitecture.puml` - NVS, FRAM and DS3231 users
- `SystemArchitectureOverview.puml` - High-level deployment of the controller
- `SystemResourceProvider.puml` - SRP static accessors and SystemInitializer
- `SystemStateLogic.puml` - System operating modes, initialization, emergency stop
- `TaskOrchestration.puml` - FreeRTOS tasks, cores, priorities, stacks and data flow: 32 live, 18 of ours running, 2 that self-delete after holding their stack through boot

See `plantuml/CONVERSION_GUIDE.md` for PlantUML syntax notes and conventions.

## Viewing

Use a PlantUML renderer:
- VS Code "PlantUML" extension
- `plantuml -tsvg plantuml/<Diagram>.puml` (run from this directory so `!include theme.puml` resolves)
- https://www.plantuml.com/plantuml/

## Current Documentation

For up-to-date architecture information, see:
- [../TASK_ARCHITECTURE.md](../TASK_ARCHITECTURE.md) - Current task structure
- [../STATE_MACHINES.md](../STATE_MACHINES.md) - Current state machines
- [../EVENT_SYSTEM.md](../EVENT_SYSTEM.md) - Event architecture
