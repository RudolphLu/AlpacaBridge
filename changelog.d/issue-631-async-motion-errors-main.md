### Fixed
- **Celestron: preserve async slew, Park and FindHome failures** (AlpacaCore, issue #631): failed or malformed handset/motor-controller status polls, timeouts and completion handling are retained through the completion properties, and failure cleanup records stop errors.
- **SynScan: preserve async Park failures** (AlpacaCore, issue #631): Park dispatch, status polling, timeout and tracking-stop failures are retained through Slewing; malformed handset status replies are rejected.
- **SkyWatcher: preserve Park and FindHome failures** (AlpacaCore, issue #631): both FindHome paths and Park retain their failures, fence stale bodies, attempt both axis stops and report when cleanup fails.
- **iOptron: fence async slew error ownership** (AlpacaCore, issue #631): accepted retries clear the previous error before worker preparation, while stale workers cannot overwrite newer operation state.
- **Bisque: propagate TheSkyX slew-completion errors** (AlpacaCore, issue #631): failed and malformed IsSlewComplete replies no longer appear as successful slew completion.
- **ZWO: preserve failed-slew state on redundant connect refresh** (AlpacaCore, issue #631): a Connected=true refresh while already connected does not clear the stored motion failure.

### Breaking changes
- **Failed telescope completion checks now return DriverException** (Celestron, SynScan, SkyWatcher and Bisque; issue #631): clients that previously received a normal false/complete answer after a failed motion-status query must handle the Alpaca error before continuing.

### Added (tests)
- **Async telescope motion-failure regressions** (issue #631): cover Celestron handset and motor-controller polling, timeout, Park and home failures; SynScan Park failures; SkyWatcher Park and both FindHome paths; iOptron stale-worker retry ownership; Bisque completion replies; and ZWO redundant-connect recovery semantics.
