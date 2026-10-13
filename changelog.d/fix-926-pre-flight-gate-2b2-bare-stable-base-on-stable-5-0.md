### Fixed
- **Pre-flight gate 2b2 with a bare stable base** (`scripts/ci_preflight.sh`, issue #926): `PREFLIGHT_BASE=stable/5.1` passed `--stable-remote stable` to `merge_down.py`, which looked for `stable/stable/5.0` and did not exempt a merge down that CI exempts. The gate now passes the base prefix only when `git remote` lists it, otherwise `origin`; `origin/stable/5.1` behaves as before.
