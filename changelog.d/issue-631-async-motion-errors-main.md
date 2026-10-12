### Fixed
- **Celestron: preserve async slew, Park and FindHome failures** (AlpacaCore, issue #631): failed or malformed handset/motor-controller/MC_LEVEL_DONE status polls, timeouts and completion handling are retained through the completion properties, and failure cleanup records stop errors.
- **SynScan: preserve async Park failures** (AlpacaCore, issue #631): Park dispatch, status polling, timeout and tracking-stop failures are retained through Slewing; malformed handset status replies are rejected.
- **SkyWatcher: preserve Park and FindHome failures** (AlpacaCore, issue #631): both FindHome paths and Park retain their failures, fence stale bodies, attempt both axis stops and report when cleanup fails.
- **iOptron: serialize async slew handoffs** (AlpacaCore, issue #631): RA/Dec and Alt/Az initiators cannot reap a newer caller's dispatch; accepted retries clear prior failures before worker preparation, while stale workers cannot overwrite newer operation state.
- **Bisque: propagate TheSkyX slew-completion errors** (AlpacaCore, issue #631): failed and malformed IsSlewComplete replies no longer appear as successful slew completion.
- **ZWO: preserve failed-slew state on redundant connect refresh** (AlpacaCore, issue #631): a Connected=true refresh while already connected does not clear the stored motion failure.

### Breaking changes
- **Failed telescope completion checks now return DriverException** (Celestron, SynScan, SkyWatcher, Bisque and ZWO; issue #631): clients that previously received a normal false/complete answer after a failed motion-status query, or whose redundant Connected=true cleared that failure, must handle the Alpaca error before continuing.

### Added (tests)
- **Async telescope motion-failure regressions** (issue #631): cover Celestron malformed full HC replies, MC_LEVEL_DONE, timeout, Park/Home failures and both slew completion-tail paths; SynScan Park polling/tracking-stop errors after its 8 s force window; SkyWatcher partial Park dispatch and verified two-axis cleanup; iOptron pre-reap initiator interleaving; Bisque completion replies; and ZWO redundant-connect recovery semantics.
