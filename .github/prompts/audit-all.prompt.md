---
description: "Run a full QA audit of all OBCController firmware source files against all instruction files. Use when: running a full compliance check; pre-commit audit; reviewing all source files at once for rule violations and bugs."
mode: agent
---
You are acting as the Firmware QA auditor. Audit every source file in `firmware/main/` against all applicable instruction files in `.github/instructions/`.

## Files to Audit (in order)

1. `firmware/main/gpio_config.h`
2. `firmware/main/main.c`
3. `firmware/main/twai_handler.c` + `firmware/main/twai_handler.h`
4. `firmware/main/mcpwm_capture.c` + `firmware/main/mcpwm_capture.h`
5. `firmware/main/adc_reader.c` + `firmware/main/adc_reader.h`
6. `firmware/main/state_machine.c` + `firmware/main/state_machine.h`
7. `firmware/main/rx_dispatcher.c` + `firmware/main/rx_dispatcher.h`
8. `firmware/sdkconfig.defaults`
9. `firmware/main/CMakeLists.txt`

## Instructions

For each file or file pair above:
1. Load the relevant instruction files (use the mapping in the Firmware QA agent).
2. Read the source file(s) in full.
3. Report all findings using the standard severity format: CRITICAL / VIOLATION / WARNING / NOTE.
4. Move to the next file.

## Final Summary

After all files are audited, output a **Summary Table**:

| File | CRITICAL | VIOLATION | WARNING | NOTE |
|------|----------|-----------|---------|------|
| gpio_config.h | N | N | N | N |
| ... | | | | |

End with an overall compliance verdict: **PASS** (zero CRITICAL or VIOLATION), **FAIL** (one or more CRITICAL or VIOLATION), or **REVIEW** (only WARNINGs/NOTEs).
