### Changed (docs)
- **Merge downs use `git rerere`** (`.claude/commands/submit-pr.md`): the merge-down block turns on `rerere.enabled` before `git merge`, so the `VERSION`, README badge and changelog-fragment conflicts that every merge down repeats are recorded once and replayed after that. `rerere.autoupdate` stays off, and `check_docs_drift.py` is still the check for the version files.
