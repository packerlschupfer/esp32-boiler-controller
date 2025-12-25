---
description: Analyze main boiler controller project with library awareness
---

**Comprehensive analysis of the main boiler controller project:**

## 1. Read Library Summary First
- Read ~/git/vscode-workspace-libs/README.md for library changes
- Note any breaking changes or new features
- Identify which improved libraries are used here

## 2. Project Structure Analysis
- Understand the main.cpp and overall architecture
- Map out all modules and their interactions
- Identify which libraries are used where
- Check platformio.ini for library versions

## 3. Integration Impact Assessment
- List all library imports and usages
- Check if library API changes affect the code
- Identify deprecated patterns from library improvements
- Find opportunities to use new library features

## 4. Code Quality Review
- Apply same improvement patterns as libraries:
  * Thread safety issues
  * Resource management
  * Error handling
  * Const correctness
- Look for project-specific issues:
  * Task priorities and stack sizes
  * Inter-task communication
  * State machine logic
  * MQTT topic management

## 5. Create Improvement Plan
Create a detailed IMPROVEMENTS.md with:
- Integration fixes needed (from library changes)
- Code quality improvements
- Architecture enhancements
- Performance optimizations
- Safety and reliability improvements

## 6. Documentation Needs
- Update main README.md
- Document system architecture
- Create deployment guide
- Add troubleshooting section

Categorize all findings by:
- 🔴 Critical: Breaking changes from libraries
- 🟡 High: Safety and reliability
- 🟢 Medium: Performance and quality
- 🔵 Low: Nice-to-have improvements

Output a structured plan before implementing anything.
