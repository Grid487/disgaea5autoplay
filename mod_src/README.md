# Disgaea 5 Autoplay source — v5.5.2

This folder contains the C++17 source, headers, export definition, and PowerShell build script for the x64 XInput proxy. See the [main README](../README.md) for installation, controls, custom parties, compatibility, and the MIT license.

## Build

Place `mod_src/` directly inside the supported Disgaea 5 Complete game folder, beside `disgaea5.exe`. Install Visual Studio 2022 Build Tools with the C++ x64 toolchain and Windows SDK. The build script uses the standard BuildTools installation path; adjust it if necessary.

Run from the game folder:

```powershell
# Compile without installing.
powershell -ExecutionPolicy Bypass -File .\mod_src\build.ps1 -StageOnly

# With the game closed: compile, back up the current DLL, and install.
powershell -ExecutionPolicy Bypass -File .\mod_src\build.ps1
```

Output and manifests go to `../mod_dev/build/<timestamp>/`. Verified previous-DLL backups go to `../mod_dev/backups/builds/`. These directories are created automatically. A normal build requires no files from `mod_dev/`.

The supported executable SHA-256 is `49A7530BAA1C3E15E4BF9C63C56FEFD71D7E51A6E641BCE80280B37B2DB282CE`. Native offsets are specific to that build.

## Local development

Tests, private research, planning notes, archived builds, and backups are kept outside this source folder in `../mod_dev/`. If those local tools are present, run the offline suites from the game folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\mod_dev\tests\run_all_tests.ps1
```

The optional `-NativeNotificationQA` build requires `../mod_dev/support/native_notification_qa.h`. It creates synthetic test notices and is for local development. Normal builds leave this flag and `-ProfilePlanning` disabled.

Only original mod source and documentation belong in the public repository. Keep `mod_dev/`, game files, saves, logs, and personal party settings private.
