status: ready
# -chainfix: the all-programs report header was written while the next program started
Root cause: after a TIMEOUT (and equally after any verdict) the host calls RequestRestartAsync. The call can return a
failure text although the restart is granted (RestartPending, or the 30 s poll expiring); the host then fell through to
FinishAllChain and wrote the report.txt header (programs=2) while the next process was already starting. The CI wait loop
stopped at the first `programs=` line, so every later program had "no block".
Fix (host-uwp/host.cpp): Impl::AwaitRestartKill waits up to 45 s to be ended after a reported restart failure, in the
TIMEOUT path and in the normal chain path; only a process that survives writes the summary. The header gains
`chain=done` (or `chain=aborted`). CI (host-uwp.yml): the wait loop waits for programs= equal to the number of chain entries or chain=.
Proof: three consecutive host-uwp dispatches green, 45 programs, 44 PASS, 1 TIMEOUT: runs 40108409309, 40109206851, 40109906257.
