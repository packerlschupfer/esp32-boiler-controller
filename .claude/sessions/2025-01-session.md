# Session Summary - January 2025

## Overview
This session focused on resolving critical memory issues, optimizing the system for different build modes, and organizing project documentation.

## Major Accomplishments

### 1. Memory Crisis Resolution
**Problem**: Stack overflow in MB8ARTControl task when logging temperature data
- Error: "Stack canary watchpoint triggered"
- Root cause: vsnprintf formatting 7 float values used excessive stack space

**Solution**: 
- Created `SafeLog.h` utilities for stack-efficient float logging
- Implemented `SafeFloatLogger::logSensorSummary()` 
- Pre-formats floats before passing to Logger

### 2. Comprehensive Memory Optimization
Implemented tiered optimization strategy by build mode:

| Build Mode | Memory Saved | Percentage | Approach |
|------------|--------------|------------|----------|
| RELEASE | 18.2 KB | 5.7% | Aggressive |
| DEBUG_SELECTIVE | 6.6 KB | 2.1% | Balanced |
| DEBUG_FULL | 11.4 KB | 3.6% | Conservative |

**Optimizations Applied**:
- Task stack sizes (carefully tuned per task requirements)
- MQTT buffers reduced (768→512, 384→256/320 bytes)
- MQTT queues reduced (5→3 high priority, 10→5 normal)
- JSON documents converted from Dynamic to Static
- Logger buffer sized by mode (512/384/256 bytes)

### 3. Logger Simplification
- Reduced from ~900 to ~300 lines of code
- Removed complex features: memory pools, context tracking, performance metrics
- Fixed mixed output issues by removing ESP-IDF log interception
- Now acts as thin wrapper around ESP-IDF logging

### 4. Global Variable Refactoring
Complete conversion to SystemResourceProvider (SRP) pattern:
- No more extern global variables
- Thread-safe access through SRP static methods
- Examples:
  ```cpp
  SRP::getLogger().log(...)
  SRP::setSystemStateEventBits(BIT)
  SRP::takeSensorReadingsMutex(timeout)
  ```

### 5. PersistentStorage MQTT Integration
- Remote parameter access via MQTT
- Topics: `esplan/params/set/*`, `esplan/params/get/*`
- NVS-backed storage with change callbacks
- Automatic save every 5 minutes

### 6. Infrastructure Improvements
Created monitoring and analysis tools:
- `tools/analyze_memory.py` - Static memory analysis
- `tools/monitor_stacks.py` - Runtime stack monitoring  
- `tools/calculate_savings_v2.py` - Memory savings by mode
- `tools/memory_report.md` - Detailed memory analysis

## Key Learnings

1. **Float formatting is expensive**: vsnprintf with %.1f uses 50-100 bytes of stack per float
2. **Conservative is better**: Initial aggressive optimization caused crashes
3. **Build modes matter**: Different use cases need different optimization levels
4. **Pre-formatting helps**: Breaking up complex format operations reduces peak stack usage

## Critical Information for Future Sessions

### Stack Size Requirements
- MB8ARTControl/Status tasks: Minimum 3584 bytes (logs multiple floats)
- Any task using LOG_* with %.1f: Add ~512 bytes safety margin
- Monitor with `python3 tools/monitor_stacks.py`

### Memory Status
- Static RAM usage: ~48KB (14.6% of 320KB)
- Free heap at runtime: 140-160KB
- Minimum free heap: 120-130KB (healthy margin)

### Fixed Issues
- Temperature display shows correct precision (24.6 not 24.60000038)
- Relay 4/5 mapping corrected (were swapped)
- Logger no longer causes mixed output
- Stack overflow in float logging resolved

## Files Organized
Moved documentation from root to `.claude/` structure:
- `.claude/docs/` - Active documentation
- `.claude/archive/` - Completed work
- `.claude/logs/` - Log files
- `.claude/sessions/` - Session summaries