# Architecture Diagrams

**Note**: These diagrams are checked against the code by hand and can lag behind it. Cross-check with the current documentation and the code before relying on details.

## Diagrams

All sources are PlantUML files in `plantuml/`; each one includes the shared dark theme `plantuml/theme.puml`.

- `BurnerStateMachine.puml` - Burner state machine: 9 states and their transitions
- `BurnerSystemController.puml` - BurnerSystemController class, burner relays R1-R3 and pump separation
- `CompleteSystemArchitecture2025.puml` - Deployment view: 19 tasks, shared resources, Modbus, storage, network, MQTT topics
- `ControlRequestFlow.puml` - Sequence: heating/water requests, water priority, demand arming, burner relays
- `HeatingControlLogic.puml` - Activity: HeatingControlTask, weather vs room mode, heating curve, burner request
- `HotWaterScheduler.puml` - Aspirational pre-heating scheduler (not implemented; TimerSchedulerTask exists)
- `LibraryEcosystem.puml` - Custom libraries and their dependencies
- `ParameterManagementFlow.puml` - Component view: MQTT parameter topics, PersistentStorage, NVS
- `ParameterUpdateSequence.puml` - Sequence: parameter set, save, get/list
- `SafetyErrorHandling.puml` - Error sources, failsafe levels, emergency stop and release, error reporting
- `SharedResourceManager.puml` - Event groups, standard mutexes and queues
- `StorageArchitecture.puml` - NVS, FRAM and DS3231 users
- `SystemArchitectureOverview.puml` - High-level deployment of the controller
- `SystemResourceProvider.puml` - SRP static accessors and SystemInitializer
- `SystemStateLogic.puml` - System operating modes, initialization, emergency stop
- `TaskOrchestration.puml` - FreeRTOS tasks, cores, priorities and data flow

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
