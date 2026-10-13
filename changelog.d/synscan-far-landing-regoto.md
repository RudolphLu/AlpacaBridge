### Breaking changes
- **A SynScan GOTO that ends more than 600" from its target twice now fails the slew** (issue #1105, SynScan): on 4.2.0 such a slew completed with the mount wherever the handset left it; now `SlewToCoordinates`/`SlewToTarget` raise `DriverException` (0x500) and, for the async forms, the next `Slewing` read does. A refused or limit-stopped GOTO and a mount that never moved are reported instead of passing as a landed slew. **After upgrading:** handle the error as a failed slew; nothing to change for a mount that reaches its targets.

### Fixed
- **SynScan repeats a GOTO the handset ends 49' off** (issue #1105, SynScan): on the EQM-35 Pro (HC 06.03.00) some GOTOs, such as Dec -45 at HA -3 from HA +9, ended 2,950-3,120" east of the aim, and the driver logged "too far for a landing refinement" and reported the slew complete. A landing beyond 600" now gets one repeat GOTO to the same target, unchanged (one aimed by the residual would land 49' west), followed by the usual landing moves; a second miss that large fails the slew, see Breaking changes. ConformU did not catch this: its `SlewToHa` positioning slews never compare the landing.

### Added (tests)
- **SynScan far-miss landing cases** (issue #1105): the landing fake can script a per-GOTO RA miss; four cases cover the repeat GOTO and the second-miss failure, sync and async (`test_synscan_goto_landing.cpp`).
