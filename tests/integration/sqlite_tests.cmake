# Included from CMakeLists.txt: the tests of the SQLite bring-up. win10xbox_guest_test() takes the guest's
# command-line arguments (a list as for PROGRAM_ARGS, '@SEMI@' for a ';' inside an argument), its stdin file and a guest-visible file to delete first and require
# afterwards from the variables _guest_args, _guest_input and _guest_file (run_guest.cmake documents them).

# The command line a guest sees (core/dispatch/process.cpp, the process parameters block): every string in it must
# be NUL-terminated, or GetCommandLineW() runs on into the next string.
set(_guest_args "one;two words;three")
win10xbox_guest_test(guest_args "C:\\guest\\guest_args.exe" 42
  "args: argc=4" "args: argv\\[2\\]=two words" "args: cmdline=.*one.*two words.*three"
  "args: CommandLine length=[0-9]+ strlen=[0-9]+ terminated=1" "args: ImagePathName length=[0-9]+ strlen=[0-9]+ terminated=1")
unset(_guest_args)

# The SQLite command-line shell, built from the official amalgamation by tools/ci/build_sqlite.ps1 into
# <machine>/guest/sqlite3.exe (the windows job of ci.yml). MSVC builds only: the pinned archive is built there, and
# a missing sqlite3.exe is a failing test, not a skipped one. Other toolchains do not register them (the Linux job has
# no sqlite3.exe); -DWIN10XBOX_SQLITE_TESTS=ON does, for a developer who staged one by hand (docs/TESTING.md).
if(MSVC)
  set(_sqlite_default ON)
else()
  set(_sqlite_default OFF)
endif()
option(WIN10XBOX_SQLITE_TESTS "Register the SQLite shell guest tests (needs <machine>/guest/sqlite3.exe)" ${_sqlite_default})
if(WIN10XBOX_SQLITE_TESTS)
  # (a) the one-liner: an argument with a space and a semicolon through the command line, an in-memory database.
  set(_guest_args "-batch;:memory:;select 6*7@SEMI@")
  win10xbox_guest_test(sqlite_memory "C:\\guest\\sqlite3.exe" 0 "^42.?$")
  unset(_guest_args)

  # (b) a script on stdin against a database file in the machine folder (C:\Users\User\smoke.db): journal files,
  # locking, fsync, directory and attribute queries, .dump, date('now'), `.exit 42`. The lines are what sqlite3
  # prints for tests/guest/sqlite/smoke.sql; the file must exist afterwards.
  set(_guest_args "-batch;C:\\Users\\User\\smoke.db")
  set(_guest_input ${CMAKE_SOURCE_DIR}/tests/guest/sqlite/smoke.sql)
  set(_guest_file Users/User/smoke.db)
  win10xbox_guest_test(sqlite_script "C:\\guest\\sqlite3.exe" 42
    "plum\\|7.pear\\|5.apple\\|3.fig\\|2"
    "4\\|17\\|2\\.0"
    "pear\\|3\\.75.plum\\|1\\.75.apple\\|1\\.5"
    "indexed\\|1"
    "BEGIN TRANSACTION;.CREATE TABLE fruit\\(id integer primary key, name text not null, qty integer, price real\\);"
    "INSERT INTO fruit VALUES\\(1,'apple',3,0\\.5\\);.INSERT INTO fruit VALUES\\(2,'pear',5,0\\.75\\);"
    "INSERT INTO fruit VALUES\\(4,'plum',7,0\\.25\\);.CREATE INDEX fruit_name on fruit\\(name\\);.COMMIT;"
    "date\\|10\\|1\\|1.ok")
  unset(_guest_args)
  unset(_guest_input)
  unset(_guest_file)
endif()

# Byte-range locking (core/dispatch/nt_file.cpp, NtLockFile/NtUnlockFile): Windows rejects the NULL IoStatusBlock that
# Wine's kernelbase passes, and Wine hosts of different versions disagree about NtUnlockFile's.
win10xbox_guest_test(guest_lockfile "C:\\guest\\guest_lockfile.exe" 42
  "lockfile: exclusive lock 10\\+100 on first handle -> 1" "lockfile: same range on second handle -> 0 error=33"
  "lockfile: shared lock, same range, second handle -> 1" "lockfile: exclusive over the shared range -> 0 error=33"
  "lockfile: LockFile same, second handle -> 0 error=33" "lockfile: done")
