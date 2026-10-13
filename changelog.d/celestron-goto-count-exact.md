### Changed (tests)

- **Celestron retried async slew again requires exactly one GOTO** (AlpacaCore tests, PR #857): the case "Celestron async - a GOTO the hand controller never acknowledges surfaces through Slewing (#575)" waits for `goto_count == 1` again instead of `> 0`, so a duplicate GOTO on the retried slew fails the test. PR #857 relaxed the wait without a stated reason; the driver sends one GOTO (30 of 30 runs pass on arm64).
