### Changed (tooling)
- **Review bot: pauleyj added to the fork-contributor allowlist** (CI, `.github/workflows/claude-review.yml`): `allowed_non_write_users` now includes `pauleyj`, so the Claude review runs on their own pushes to a fork PR carrying the `safe-to-review` label instead of failing the action's write-access check.
