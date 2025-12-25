# Claude Session Initialization Guide

## Starting a New Claude Code Session

### 1. Initial Context
When starting a new session, provide Claude with:

```
I'm working on the ESPlan boiler controller project. 
Project path: /home/mrnice/Documents/PlatformIO/Projects/ESPlan-blueprint-Boiler-Controller-MB8ART-workspace

Please read:
1. .claude/claudeInstructions.txt - for critical patterns and guidelines
2. .claude/docs/CLAUDE.md - for project overview and recent changes
3. .claude/sessions/ - check the most recent session summary

Key points:
- Use SystemResourceProvider (SRP) pattern - no global variables
- Tasks with float logging need adequate stack (min 3584 bytes)
- Build mode: esp32dev_usb_debug_selective
- Monitor stack usage if making memory changes
```

### 2. Check Recent Work
Ask Claude to review recent changes:
```
Please check .claude/tasklog.txt for recent work completed.
What was the last session about?
```

### 3. Verify Build Status
Start with a clean build check:
```
Please run: pio run -e esp32dev_usb_debug_selective
Are there any compilation errors or warnings?
```

### 4. Key Technical Reminders

**Memory Management:**
- Use SafeLog.h for multiple float logging
- Stack sizes are tuned by build mode
- Monitor with tools/monitor_stacks.py

**Code Patterns:**
- SRP::getLogger() instead of extern logger
- Check sensor validity flags before use
- Feed watchdog during long operations

**Common Issues:**
- Stack overflow: "Stack canary watchpoint triggered"
- MQTT needs 2s after connection before subscriptions
- Service resolution needs explicit names

### 5. Library Locations
- Project libs: /home/mrnice/Documents/PlatformIO/libs/workspace_*
- External libs: .pio/libdeps/

### 6. Testing Tools
```bash
# Monitor system
python3 tools/monitoring/monitor_system.py -t 120

# Check stack usage
python3 tools/monitor_stacks.py

# Analyze memory
python3 tools/analyze_memory.py
```

### 7. Session Workflow

1. **Start**: Read initialization files
2. **Check**: Verify build status
3. **Plan**: Discuss objectives
4. **Implement**: Make changes
5. **Test**: Compile and verify
6. **Document**: Update tasklog.txt
7. **Commit**: Git commit with co-author

### 8. Important Files to Know

- `src/config/ProjectConfig.h` - Stack sizes, build modes
- `src/utils/SafeLog.h` - Float logging utilities
- `src/core/SystemResourceProvider.h` - Global resource access
- `src/common/SystemStrings.h` - Common error strings

### 9. Current System State (Jan 2025)

- Memory optimized (saves 6-18KB by mode)
- Logger simplified to 300 lines
- All globals use SRP pattern
- PersistentStorage integrated with MQTT
- vsnprintf stack issue fixed with SafeLog

### 10. Git Commit Format
```bash
git commit -m "type: description

- Detail 1
- Detail 2

Co-Authored-By: Claude <noreply@anthropic.com>"
```

Types: feat, fix, docs, refactor, test, chore