---
description: Integrate all library improvements into main project
---

**AUTONOMOUS MODE: Work through all integration tasks systematically**

## 1. Pre-Integration Checks
- Git status - ensure clean working directory
- Read ~/git/vscode-workspace-libs/README.md
- Check platformio.ini library paths

## 2. Update Library Imports
For each improved library:
- Check if used in this project
- Update any changed API calls
- Apply new best practices from library examples
- Fix any compilation errors

## 3. Apply Library Patterns
Based on library improvements, update main project:

### Thread Safety
- Apply SemaphoreGuard pattern where needed
- Fix any unprotected shared resources
- Verify task synchronization

### Error Handling
- Use improved error codes from libraries
- Add proper error propagation
- Implement recovery mechanisms

### Resource Management
- Apply RAII patterns from libraries
- Fix any memory leaks
- Proper cleanup in destructors

### Const Correctness
- Apply const to methods not modifying state
- Use const references for parameters
- Mark member variables const where appropriate

## 4. Update Module Integration
For each major module:
- **HeatingControlModule**: Use improved MB8ART features
- **WaterControlModule**: Apply new safety patterns
- **BurnerControlModule**: Integrate RYN4 improvements
- **MQTTManager**: Use enhanced MQTT features
- **TaskManager**: Apply improved task patterns

## 5. Configuration Updates
- Update system_config.h with new parameters
- Apply improved logging patterns
- Update WiFi/Ethernet configuration

## 6. Testing Points
Document what needs testing:
- Library integration points
- Task communication
- Error recovery paths
- MQTT connectivity
- Modbus communication

## Git Strategy
Group commits by integration area:
- "feat: Integrate improved MB8ART and RYN4 libraries"
- "refactor: Apply thread safety improvements from libraries"
- "fix: Update API calls for library changes"
- "docs: Update documentation for new library features"

Work autonomously, make decisions based on library patterns.
