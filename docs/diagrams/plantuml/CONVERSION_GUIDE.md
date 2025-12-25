# PlantUML Conversion Guide

## Completed Conversions ✅

1. ✅ **LibraryEcosystem.puml** - Component diagram with 18 libraries
2. ✅ **BurnerSystemController.puml** - Class diagram with H1 refactoring
3. ✅ **BurnerStateMachine.puml** - State diagram with 8 states
4. ✅ **SystemResourceProvider.puml** - Service Locator pattern
5. ✅ **SharedResourceManager.puml** - Event groups and mutexes

## Remaining Conversions

### High Priority
- [ ] **StorageArchitecture.puml** - Component diagram for NVS/FRAM/RTC
- [ ] **TaskOrchestration.puml** - Component/deployment showing 16 tasks
- [ ] **CompleteSystemArchitecture2025.puml** - Deployment diagram with all layers

### Medium Priority
- [ ] **ParameterManagementFlow.puml** - Component diagram
- [ ] **ControlRequestFlow.puml** - Sequence diagram
- [ ] **HeatingControlLogic.puml** - Activity diagram
- [ ] **SafetyErrorHandling.puml** - Component/flow diagram

### Lower Priority
- [ ] **SystemArchitectureOverview.puml** - High-level deployment
- [ ] **SystemStateLogic.puml** - State diagram
- [ ] **ParameterUpdateSequence.puml** - Sequence diagram
- [ ] **HotWaterScheduler.puml** - State diagram (future feature)

## Conversion Tips

### 1. Choose the Right Diagram Type

- **Component Diagram**: For showing system components and their relationships
  - LibraryEcosystem, StorageArchitecture, ParameterManagement

- **Class Diagram**: For detailed class structure with methods/attributes
  - BurnerSystemController, SystemResourceProvider

- **State Diagram**: For state machines and transitions
  - BurnerStateMachine, SystemStateLogic, HotWaterScheduler

- **Sequence Diagram**: For message/call flow over time
  - ControlRequestFlow, ParameterUpdateSequence

- **Deployment Diagram**: For hardware/physical deployment
  - CompleteSystemArchitecture2025, TaskOrchestration (can also use component)

- **Activity Diagram**: For process flow/algorithms
  - HeatingControlLogic, SafetyErrorHandling

### 2. Common PlantUML Syntax

```plantuml
@startuml DiagramName
!include theme.puml

title Your Diagram Title

' Component Diagram
component [Component Name] as Alias
package "Package Name" {
    component [Comp1]
    component [Comp2]
}

Comp1 --> Comp2 : uses
Comp1 ..> Comp2 : depends on

' Class Diagram
class ClassName {
    - privateField: type
    + publicMethod(): returnType
}

class Child extends Parent
class Implementation implements Interface

' State Diagram
state StateName {
    StateName : Entry: action
    StateName : Do: activity
    StateName : Exit: cleanup
}

[*] --> State1
State1 --> State2 : event [condition]

' Sequence Diagram
participant Actor
Actor -> System : message
System --> Actor : response

' Notes
note right of Component
    Additional information
    or explanation
end note

@enduml
```

### 3. Using the Theme

Always include at the top:
```plantuml
@startuml
!include theme.puml
```

This applies the dark theme matching the previous Mermaid diagrams.

### 4. Stereotypes for Visual Distinction

```plantuml
class ClassName <<Singleton>>
component [Service] <<Interface>>
state ErrorState <<choice>>
```

### 5. Layout Hints

```plantuml
' Force left-to-right layout
left to right direction

' Grouping
package "Group Name" {
    ' components
}

' Manual positioning (if needed)
Component1 -[hidden]-> Component2
```

## Viewing PlantUML Diagrams

### Online
- **PlantUML Online**: http://www.plantuml.com/plantuml/uml/
- **PlantText**: https://www.planttext.com/

### Local Tools
- **VS Code**: Install "PlantUML" extension by jebbs
- **IntelliJ/WebStorm**: Built-in PlantUML support
- **Command Line**: `plantuml diagram.puml` (generates PNG/SVG)

### GitHub
GitHub doesn't render PlantUML natively, but you can:
1. Generate PNG/SVG files and commit them
2. Use PlantUML Proxy: `![diagram](http://www.plantuml.com/plantuml/proxy?src=https://raw.githubusercontent.com/.../diagram.puml)`

## Export Options

```bash
# Generate PNG
plantuml diagram.puml

# Generate SVG (recommended for docs)
plantuml -tsvg diagram.puml

# Generate both
plantuml -tpng -tsvg diagram.puml

# Watch for changes
plantuml -watch diagram.puml
```

## Migration Checklist

When converting a Mermaid diagram:

1. [ ] Choose appropriate PlantUML diagram type
2. [ ] Create `.puml` file with `!include theme.puml`
3. [ ] Convert main structure (components/classes/states)
4. [ ] Convert relationships/connections
5. [ ] Add notes from original diagram
6. [ ] Add any additional detail PlantUML enables
7. [ ] Test rendering locally
8. [ ] Delete original `.mmd` file
9. [ ] Update README.md

## Notes

- PlantUML is more verbose but much more powerful
- Take advantage of additional detail capabilities
- Use proper UML notation where appropriate
- Keep diagrams focused - split if too complex
