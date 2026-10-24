# Briefs for one-shot laptop agents

One file per slice, run by Session C on the owner's Windows machine with
`tools/dev/run-agents.ps1 -Briefs docs\briefs\<slice>.md` (reports land in `reports\`), or directly with
`claude -p --model claude-sonnet-5-5 (Get-Content -Raw docs\briefs\<slice>.md)` in a window that has ANTHROPIC_API_KEY set.
A brief states: branch, files, contract, test, done condition, two-push budget, explicit-path staging, never push to
main, "never ask questions", and the commit trailer. Status flows back through line 1 of `docs/slices/<slice>.md`.
