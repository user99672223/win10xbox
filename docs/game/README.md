# docs/game: import lists of the game (names only)

`docs/game/*_imports.txt` hold, for each executable and DLL of the game install, the DLLs and symbols it imports. They are
the input of the closure test (`tests/guest/closure.c`, docs/slices/gameclosure.md): which imported DLLs and symbols the layer
cannot resolve.

## What to run (on the PC that has the game, python3 only, no pip)

    python3 tools/game/dump_imports.py "<game install directory>" docs/game

It walks the directory recursively, parses every x64 PE (import table, delay-load imports, ordinals as `#N`) and writes one
`<file name>_imports.txt` per file into docs/game. Run it on the game's folder (and on any launcher/anti-cheat/SDK folder
the game needs). Then commit `docs/game/*_imports.txt` and push.

## What is committed

Per file: its name, its size in bytes, and the sorted, deduplicated `<imported dll> <symbol|#ordinal>` lines. Nothing else
(format at the top of tools/game/dump_imports.py).

## What never leaves the PC

The game's binaries, any hash, section contents, strings, resources, assets and the install path. Only the import
names above are written, and the tool reads the files without modifying them.

## Running the closure

Copy `docs/game/*_imports.txt` into a folder of the machine folder (for example `machine-native-d3d\guest\closure`) and
run `C:\guest\guest_closure.exe C:\guest\closure` through the layer (the default directory is `C:\guest\closure`). It prints
`closure: MISSING DLL <name> (needed by <module>)` and `closure: MISSING <dll>!<symbol> (<module>)`, then the summary; exit 42
when nothing is missing, 43 otherwise.
