### Fixed (tooling)

- **`/submit-pr` merge-down recipe stops when its conflict guard trips** (`.claude/commands/submit-pr.md`, issue #925): a tripped guard used to skip only `git commit`, so the branch was still pushed and an unmergeable PR opened; the commit, push and `gh pr create` now run only on the guard's else branch, and the recipe prints one `STOPPED:` line naming the unresolved paths or the staged conflict markers.
