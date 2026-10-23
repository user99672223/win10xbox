# Guests added while bringing up the SQLite shell (tools/ci/build_sqlite.ps1, tests/integration/sqlite_tests.cmake):
# one small program per layer defect it exposed, so each fix has a regression test that also runs under Wine.
add_executable(guest_args args.c)
win10xbox_add_guest(guest_args)

add_executable(guest_lockfile lockfile.c)
win10xbox_add_guest(guest_lockfile)
