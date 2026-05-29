<p align="center">
  <img src="HelixSeed.png" width="220" alt="HelixSeed">
</p>

<h1 align="center">HelixSeed</h1>

<p align="center">
GPU-assisted Minecraft seed scanning toolkit with a desktop UI and a small scripting language.
</p>

<p align="center">
Fast candidate filtering on the GPU, strict CPU-side verification, and a query language designed for humans.
</p>

---

## Overview

HelixSeed searches Minecraft world seeds for ones that contain the structures and biomes you want. It runs a fast GPU prefilter to throw away seeds that obviously do not match, then a strict CPU verification pass on the survivors so the seeds it reports really do contain what you asked for.

You drive HelixSeed from a TypeScript/Electron desktop app. Constraints can be assembled either with a point-and-click **Builder**, or written directly in **HelixScript** — a small line-oriented DSL. Both produce the same compiled query for the native scanner.

---

## Features

- GPU-accelerated candidate prefilter (CUDA, multi-GPU aware)
- Strict CPU verification on survivors
- **HelixScript** scripting language: variables, lists, for-loops, conditionals, math, functions, runtime `found` checks
- Builder UI with structure constraints and biome filters (loot filters present in the engine but currently disabled in the UI)
- One-click **Smart Mode** preset (mixed scan + maxed engine settings)
- Live telemetry: seeds-per-second, queue depth, GPU survivors, strict throughput, Java validation rejects
- In-app **User Guide** and **HelixScript Field Guide**
- Ko-fi support button

---

## Architecture

HelixSeed has three layers:

```
Electron / TypeScript UI  →  native C++ scanner  →  CUDA prefilter
       (ui-ts/)                   (native/)              (cuda/)
```

### UI layer (`ui-ts/`)

Electron + Vite + TypeScript. The Builder tab is a constraint editor; the Query tab is a HelixScript editor with linting and autocomplete; Results streams found seeds; Telemetry charts the scan; Documentation opens the in-app guide.

### Native scanner (`native/`)

C++ pipeline driven by `scanner_native.exe`. Implements three scan modes:

- **placement** — fast geometric placement test, may report false positives
- **strict** — full structure verification, no false positives
- **mixed** — placement first as a cheap filter, strict only on survivors

### CUDA prefilter (`cuda/`)

`gpu_filter.dll` — multi-GPU aware kernel that throws away the obviously-failing majority of seeds before the CPU even looks at them.

### Vendored

- `cubiomes_26.1.2_fork/` — cubiomes retargeted to Minecraft 26.1.2
- `kaptainwutax/` — SeedFinding libraries (kept for future Java validation work)
- `GPULootSeedFinder/` — loot resolver for the upcoming loot validation path

---

## HelixScript at a glance

```
let radii = [64, 96, 128]

for r in radii
  find village as v_$r within $r of origin strict
end

biome origin y 80 radius 128 allow plains, meadow, cherry_grove

if $radius of v_64 == 64
  find ruined_portal as rp1 within 96 of constraint:v_64 strict
end
```

What the language gives you:

- `find <structure> as <id> within <radius> of <anchor> [strict|placement] [optional]`
- `biome <point> y <y> radius <r> allow <list>`
- `let / const / set` variables, `$name` substitution
- Lists with `[a, b, c]`, `${list[0]}`, `.length`, `.first`, `.last`
- `for x in <list> ... end` loops
- `if / elif / else / skip / continue / pause / stop`
- `contains` operator, comparison operators, `and / or / not`
- Arithmetic in assignments: `+ - * / %`, parens, `min / max / abs / floor / ceil / round`
- Compile-time `function name(args) ... end` helpers
- `scan { ... }` and `performance { ... }` blocks for runtime tuning
- Runtime per-seed `found` checks for conditional structure rules
- Indentation and `# / //` comments are optional

The full reference lives in the in-app **HelixScript Field Guide** (Documentation tab).

---

## Installing

If you just want to run HelixSeed, grab `HelixSeedInstaller.exe` from the
[latest GitHub release](https://github.com/HelixSeed-MC/HelixSeed/releases/latest).
It is a small C++ console installer that clones the source, runs the full
build pipeline (CUDA prefilter → native scanner → loot finder → Electron UI),
and reports the path to the packaged `HelixSeed.exe`. It needs git, Node.js,
MSVC, and the CUDA Toolkit on PATH (Maven is optional).

## Building

### Requirements

- Windows 10 / 11 (x64)
- Node.js 20+ and npm
- Visual Studio Build Tools (MSVC) for the native scanner
- CUDA Toolkit (for `gpu_filter.dll`) — multi-GPU optional

### UI (from `ui-ts/`)

```
npm install
npm run build       # tsc + vite build → dist/
npm start           # build + launch Electron
npm run dist:win    # packaged Windows build → dist-ts/HelixSeed-win32-x64/
```

### Native scanner (from repo root)

```
cmd /c native\build_scanner_core.bat
cmd /c cuda\build_cuda.bat
```

The packaged Windows build under `dist-ts/HelixSeed-win32-x64/` contains everything (Electron app, scanner, CUDA prefilter, vendored cubiomes/loot finder).

---

## Running

### Packaged app

Launch `dist-ts/HelixSeed-win32-x64/HelixSeed.exe`. The first time:

1. Open the **Documentation** tab → read the User Guide.
2. In the sidebar, click the **Starter** preset, then the **Smart Mode** preset.
3. Press **Run**.

### Headless

The native scanner runs on its own:

```
native\scanner_native.exe --query-file query.json --scan-mode mixed --count 1000000
```

The UI's command preview shows the exact arguments it would launch with for the current Builder/Query configuration.

---

## Project structure

```
cuda/                   CUDA prefilter kernel + C wrapper (gpu_filter.dll)
native/                 C++ scanner core + CLI (scanner_native.exe)
ui-ts/                  Electron / TypeScript desktop UI
  src/main/             Electron main process
  src/preload/          Renderer ↔ main IPC bridge
  src/renderer/         Builder, Query, Results, Telemetry, Docs
  src/shared/           Shared TypeScript contracts
  guide.html            In-app User Guide
  docs.html             In-app HelixScript Field Guide
  scripts/              Build / packaging helpers
  assets/               UI icons (HelixSeed.png, Seed.png, kofi-mug.webp)
cubiomes_26.1.2_fork/    Vendored cubiomes (MC 26.1.2)
kaptainwutax/           Vendored SeedFinding libraries
GPULootSeedFinder/      Vendored loot resolver
benchmarks/             Performance tests
tools/                  Dev helpers
```

---

## Performance presets

The sidebar **Presets** panel has two performance presets:

- **Max Performance** — pushes every engine knob: max batch, `max-throughput` execution, multi stage A, async GPU pipeline, `lightspeed` strict surrogate, adaptive caps, automatic strict workers.
- **Smart Mode** — switches scan mode to `mixed` and applies Max Performance. The recommended "just go fast" setting.

---

## Credits

HelixSeed builds on the broader Minecraft seedfinding ecosystem.

- **cubiomes** — Cubitect and contributors. https://github.com/Cubitect/cubiomes
- **kaptainwutax SeedFinding libraries** — KaptainWutax and contributors.
- **GPULootSeedFinder** — Neil.

Thanks to the seedfinding community for years of research and tooling.

---

## Support

If HelixSeed is useful to you, you can support development on Ko-fi:

https://ko-fi.com/E1E41ZL4SR

There's a Ko-fi tab in the app's tabbar — click it twice to open the page.

---

## License

MIT License.

---

## Disclaimer

HelixSeed is an independent project and is not affiliated with Mojang or Minecraft.
