# gputxrx installer

Double-click scripts to make `gputxrx` available from any terminal.

## Install

Double-click **`install.bat`**. It:

1. Builds the project if `build\Release\gputxrx.exe` does not exist yet
   (requires CMake + Visual Studio 18).
2. Copies `gputxrx.exe` into `install\bin\`.
3. Adds `install\bin` to the **user** `PATH` (no admin rights needed).

Open a **new** terminal window afterwards and run `gputxrx --help`.
Terminals that were already open keep their old PATH.

## Uninstall

Double-click **`uninstall.bat`**. It removes the PATH entry and deletes
`install\bin`. Files elsewhere (including your `build\` directory) are
left untouched.

## Files

| File | Purpose |
|---|---|
| `install.bat` | Double-click installer (build + copy + PATH) |
| `uninstall.bat` | Double-click uninstaller (PATH + bin removal) |
| `add_to_path.ps1` | Adds a directory to the user PATH (idempotent) |
| `remove_from_path.ps1` | Removes a directory from the user PATH (idempotent) |
| `bin\` | Created by the installer; holds the deployed `gputxrx.exe` |

Note: `add_to_path.ps1` / `remove_from_path.ps1` can also be run standalone:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File add_to_path.ps1 C:\some\dir
```
