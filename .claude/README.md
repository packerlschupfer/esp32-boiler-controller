# Claude Infrastructure

This directory contains documentation and tools for Claude Code sessions.

## Structure

- **`claudeInstructions.txt`** - Key instructions and patterns for Claude
- **`tasklog.txt`** - Log of completed tasks and changes
- **`settings.local.json`** - Local Claude settings
- **`docs/`** - Active documentation
  - `CLAUDE.md` - Main reference document
  - `MEMORY_OPTIMIZATIONS.md` - Current memory optimization details
  - Other active docs
- **`archive/`** - Completed/historical documentation
- **`logs/`** - System logs and debugging output
- **`sessions/`** - Session summaries
- **`commands/`** - Command templates

## Quick Start

1. Read `claudeInstructions.txt` for critical patterns
2. Check `docs/CLAUDE.md` for project overview
3. Review recent session in `sessions/2025-01-session.md`
4. Check `tasklog.txt` for completed work

## Key Points for Claude

- Use SystemResourceProvider (SRP) pattern - no globals
- Monitor stack usage with `tools/monitor_stacks.py`
- Use SafeLog.h for float logging to prevent stack overflow
- Build modes: DEBUG_FULL, DEBUG_SELECTIVE, RELEASE
- Serial port: /dev/ttyACM0, baud: 115200