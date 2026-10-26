---
description: Continue improving based on previous work with task logging
---

**Task Logging Setup:**
1. Read tasklog.txt to understand previous improvement sessions
2. If .claude/claudeInstructions.txt exists, check for any updated instructions
3. Append new session start to tasklog.txt with timestamp
4. Note this is an iteration/continuation session

Review the recent improvements made to this library and:

1. **Check Previous Work**
   - Read tasklog.txt to see all previous improvements
   - Review recent git commits: `git log --oneline -20`
   - Verify all previous improvements were properly implemented
   - Look for any issues introduced by the changes
   - Check for any "Remaining TODOs" from last session

2. **Identify Next Level Improvements**
   - Now that basic improvements are done, look for:
     * Advanced optimizations
     * Additional safety checks  
     * Performance enhancements
     * API ergonomics improvements
     * Additional examples or test cases
     * Cross-cutting concerns missed in first pass
     * Integration improvements with other libraries
     * Advanced error recovery patterns

3. **Check for Patterns**
   - Review improvements made to other related libraries
   - Apply similar enhancements if applicable
   - Look for library-specific optimization opportunities
   - Consider architectural improvements now that basics are solid

4. **Implement Additional Improvements**
   - Focus on enhancements not covered in first pass
   - Add any missing edge case handling
   - Improve code patterns based on the example usage
   - Each improvement gets its own git commit
   - Continue using conventional commit messages

5. **Refine Documentation**
   - Update README with any new insights
   - Add troubleshooting section if needed
   - Include performance considerations
   - Document any breaking changes or migration notes
   - Update API reference with new methods/options

6. **Final Steps**
   - Update CHANGELOG.md with new improvements
   - Show git log --oneline -10 for this session
   - Append to tasklog.txt:
     * Timestamp of completion
     * List of additional improvements made
     * Git commit hashes for this session
     * Updated list of remaining TODOs
     * Suggestions for future iterations

**State Awareness:**
- Build on previous improvements, don't repeat them
- Check if previous TODOs were addressed elsewhere
- Note any patterns that could apply to other libraries
- Mark this as "Iteration N" in tasklog.txt

Work autonomously but note any architectural decisions that might affect other libraries.
