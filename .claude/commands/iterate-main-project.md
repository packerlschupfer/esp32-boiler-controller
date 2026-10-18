---
description: Continue improving the main boiler controller project
---

**Task Logging Setup:**
1. Read .claude/tasklog.txt to understand previous improvement sessions
2. Check IMPROVEMENTS_IMPLEMENTED.md for what was already done
3. Append new session start to .claude/tasklog.txt with timestamp
4. Note this is an iteration on the main project

**Review Previous Work:**
1. **Check Implementation Status**
   - Read IMPROVEMENTS_IMPLEMENTED.md
   - Review git log for recent changes
   - Check if all planned improvements were completed
   - Verify system still compiles and runs

2. **Scan for New Opportunities**
   Now that basics are done, look for:
   - Performance optimizations
   - Advanced safety features
   - User experience improvements
   - Additional diagnostic capabilities
   - Code simplification opportunities
   - Test coverage gaps

3. **Check Remaining TODOs**
   - Scan code for TODO/FIXME comments
   - Review IMPROVEMENTS.md for uncompleted items
   - Check library integration for unused new features
   - Look for patterns from library improvements not yet applied

4. **System-Specific Improvements**
   Focus on boiler controller specific enhancements:
   - **Control Algorithms**: PID tuning, adaptive control
   - **Safety Systems**: Additional interlocks, fail-safes
   - **Energy Efficiency**: Optimization algorithms
   - **User Interface**: Better MQTT topics, web interface
   - **Predictive Features**: Maintenance alerts, performance trends
   - **Communication**: Redundancy, error recovery
   - **Data Logging**: Historical data, trend analysis

5. **Implement Selected Improvements**
   - Choose 3-5 improvements per iteration
   - Group related changes together
   - Maintain system stability
   - Test safety-critical changes thoroughly
   - Document algorithm changes

6. **Update Documentation**
   - Update IMPROVEMENTS_IMPLEMENTED.md
   - Modify relevant documentation
   - Add to CHANGELOG.md
   - Update any affected deployment guides

7. **Final Steps**
   - Run test compilation
   - Update .claude/tasklog.txt with:
     * Completion timestamp
     * Improvements implemented this iteration
     * Performance impact notes
     * Remaining opportunities
     * Suggested focus for next iteration

**Iteration Guidelines:**
- Each iteration should be focused (3-5 improvements)
- Don't break working systems
- Test safety features extensively
- Consider real-world operating conditions
- Balance features with reliability

**Safety First:**
- Never compromise safety systems
- Test all control logic changes
- Verify failsafe operations
- Document safety-related changes clearly

Work autonomously but flag any changes that could affect safety or core functionality.
