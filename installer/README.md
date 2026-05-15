# HelixSeed Installer

A small, single-file Win32 installer that clones HelixSeed and builds it from
source.

## What it does

1. Detects the toolchain it needs (`git`, `npm`, `cl`, `nvcc`, `javac`, optionally `mvn`).
2. Asks where to install (defaults to `%LOCALAPPDATA%\HelixSeed`).
3. `git clone --depth=1 https://github.com/HelixSeed-MC/HelixSeed`.
4. Runs the build pipeline in order:
   - `cuda\build_cuda.bat` - CUDA prefilter (`gpu_filter.dll`)
   - `native\build_scanner_core.bat` - native scanner (`scanner_native.exe`)
   - `cubiomes_12111_fork\build_windows.bat` - native cubiomes library (`lib.dll`)
   - `GPULootSeedFinder` via `mvn.cmd`, or the bundled `mvnw.cmd` if Maven is absent
   - `npm install` and `npm run dist:win` in `ui-ts`
5. Reports the path of the packaged `HelixSeed.exe` and offers to launch it.

The installer never silently installs toolchains for you. If a prerequisite is
missing, it points you at the upstream installer URL and exits.

## Building the installer

You need the MSVC toolchain. From a Developer Command Prompt, or any shell where
`cl.exe` is available:

```bat
cd installer
build.bat
```

That produces `HelixSeedInstaller.exe` in this directory.

### Optional Code Signing

Unsigned installers have poor Windows SmartScreen and antivirus reputation,
especially when they clone source code and launch build tools. To sign the
installer during the build, set one of these before running `build.bat`:

```bat
set HELIXSEED_SIGN_PFX=C:\path\to\certificate.pfx
set HELIXSEED_SIGN_PASSWORD=your-pfx-password
build.bat
```

or, for a certificate already in the Windows certificate store:

```bat
set HELIXSEED_SIGN_SHA1=YOUR_CERT_THUMBPRINT
build.bat
```

An EV code-signing certificate gives the best SmartScreen result. A standard
OV certificate is still useful, but reputation usually builds gradually.

## Running

```bat
HelixSeedInstaller.exe
```

It installs to `%LOCALAPPDATA%\HelixSeed` by default and launches the app at the
end.

## Required Tools

| Tool | Purpose | Where to get it |
| --- | --- | --- |
| git | Clone source | https://git-scm.com/download/win |
| Node.js | Build the Electron UI | https://nodejs.org/ |
| MSVC | Compile native code | Visual Studio Build Tools |
| CUDA | Compile the GPU prefilter | https://developer.nvidia.com/cuda-toolkit |
| JDK 16+ | Build/run loot and Java validation | https://learn.microsoft.com/java/openjdk/download |
| Maven | Optional; `mvnw.cmd` is used if absent | https://maven.apache.org/ |

The installer can discover Visual Studio Build Tools through `vswhere`, so it
does not need to be launched from a Developer Command Prompt.

## Why a custom installer?

The HelixSeed build has several discrete phases: CUDA, native C++, cubiomes,
Java, and Node/Electron. The installer is intentionally a thin wrapper around the
existing build scripts rather than duplicating their logic.
