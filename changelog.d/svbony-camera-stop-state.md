### Fixed
- **SVBONY capture-stop recovery and error reporting** (AlpacaCore, issue #913): do not issue a cleanup stop when capture never started, expose an unconfirmed stop as `CameraState.Error` with zero completion, and log cleanup-stop failures.
