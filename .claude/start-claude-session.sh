#!/bin/bash
# Save as start-claude-session.sh

# Run the init script first
./.claude/init_session.sh

# Then start Claude Code
echo
echo "=== Starting Claude Code ==="
claude
