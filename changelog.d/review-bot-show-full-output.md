### Changed (tooling)
- **Review bot: `show_full_output` turned on temporarily** (CI, `.github/workflows/claude-review.yml`): every review run since 2026-10-11 14:28 UTC ends two seconds after init with `is_error: true` and no model usage, and the action hides the API error by default. The full output is printed so the next run shows the real error; revert once it is known.
