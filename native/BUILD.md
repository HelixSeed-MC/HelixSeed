# Native Scanner Core Build

This project includes a C++ validation backend that runs strict/query seed validation in native code.

## Linux

Prerequisites:

- `g++` with C++20 support
- `cmake`
- Java 16+ for loot-validation workflows

Build from repository root:

```bash
./native/build_linux.sh
```

Outputs:

- `native/scanner_core.so`
- `native/scanner_native`
- `native/scanner_backend`

## Windows

Build from repository root:

```powershell
cmd /c native\build_scanner_core.bat
```

Outputs:

- `native\scanner_core.dll`
- `native\scanner_native.exe`
- `native\scanner_backend.exe`

## Runtime Notes

- The launcher is native-only and does not invoke Python fallbacks.
- Linux launcher candidates:
  - `native/scanner_native`
  - `scanner_native`
- Windows launcher candidates:
  - `native\scanner_native.exe`
  - `scanner_native.exe`
