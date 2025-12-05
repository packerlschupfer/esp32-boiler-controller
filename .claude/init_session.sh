#!/bin/bash
# Quick initialization script for new Claude sessions

echo "=== ESPlan Boiler Controller - Session Initialization ==="
echo
echo "Project Path: $(pwd)"
echo "Build Mode: esp32dev_usb_debug_selective"
echo

# Check if we're in the right directory
if [ ! -f "platformio.ini" ]; then
    echo "ERROR: Not in project root directory!"
    exit 1
fi

echo "=== Recent Work (last 20 lines of tasklog) ==="
tail -20 .claude/tasklog.txt
echo

echo "=== Build Status Check ==="
read -p "Do you want to check build status? (y/N): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Running: pio run -e esp32dev_usb_debug_selective"
    pio run -e esp32dev_usb_debug_selective 2>&1 | tail -10
else
    echo "Skipping build check..."
fi
echo

echo "=== Key Reminders ==="
echo "1. Use SRP pattern - no global variables"
echo "2. Tasks with float logging need 3584+ bytes stack"
echo "3. Use SafeLog.h for multiple float logging"
echo "4. Monitor stack: python3 tools/monitor_stacks.py"
echo "5. Check docs: .claude/docs/CLAUDE.md"
echo

echo "=== Git Status ==="
git status --short
echo

echo "Ready to start session!"
echo "Next steps:"
echo "1. Read .claude/claudeInstructions.txt"
echo "2. Check .claude/docs/CLAUDE.md"
echo "3. Review recent session in .claude/sessions/"
