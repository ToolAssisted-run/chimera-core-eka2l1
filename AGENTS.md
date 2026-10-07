# AGENTS.md - EKA2L1 core for Chimera

This repository builds EKA2L1 (Symbian OS and the Nokia N-Gage) as a sandboxed
guest for [Chimera](https://github.com/ToolAssisted-run/chimera), a frontend
for tool-assisted speedruns, and packs it into one file, `eka2l1.chimeraCore`.
Upstream EKA2L1 is a pinned submodule; everything here is the patch series,
the adapter around the emulator, the build scripts and the gate. The core is
C++ built with CMake. Builds run on Linux, and the one package works on Linux
and on Windows.

## Layout

- `extern/eka2l1` - upstream EKA2L1, a submodule pinned by commit, with many
  submodules of its own.
- `patches/` - the numbered patch series applied to `extern/eka2l1` by
  `waterbox/apply-patches.sh`, which refuses a partly patched tree.
  `patches/mesa/` - five patches for the Mesa tarball (`setup-mesa.sh`).
- `waterbox/CMakeLists.txt` - the top level both flavors configure; upstream
  is added underneath it. `configure-flags.sh` - the shared option set
  (sourced, never run). `guest-toolchain.cmake` - the guest toolchain.
- `waterbox/machine.*`, `vclock.*`, `memfs.*`, `input.*`, `audio.*`,
  `gl-*`, `null-graphics.*`, `host-ui.*` - the adapter: the machine, its
  clock, its file system, its devices. `wbx-entry.cpp` - the guest's exports.
  `guest.mk` - compiles the adapter for the guest.
- `waterbox/run-native.cpp`, `install-device.cpp` - the native reference and
  its device installer. `run-wbx.c` - the runner that loads `core.wbx` through
  miniBox without the frontend.
- `waterbox/setup-mesa.sh`, `setup-ffmpeg.sh`, `build-native.sh`,
  `build-difftest.sh`, `build-guest.sh`, `build-core.sh`, `build-package.sh` -
  the builds. `waterbox/run-gate.sh` - the gate.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json` - what the package declares.
- `docs/PLAN.md` - the design log. `.github/workflows/chimera.yml` - CI, the
  authoritative build recipe.
- `build/`, `waterbox/bin/`, `waterbox/obj-guest/`, `waterbox/tests/work/` -
  build output. `tests/roms-local/` - the user's own ROM and games.

## Set up the build environment

`<chimera>` is a Chimera checkout and `<minibox>` is
`<chimera>/extern/chimera-common-minibox`.

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 mono-complete \
  libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev \
  libsdl2-dev libwayland-dev \
  python3-mako bison flex
git submodule update --init --recursive
git clone https://github.com/ToolAssisted-run/chimera <chimera>
git -C <chimera> submodule update --init --recursive extern
mb=<minibox>
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

The submodule update must be recursive, or CMake fails at configure time. The
contract tests also need Chimera's natives and .NET 8.0 (`docs/BUILDING.md`).

## Build

```sh
sh waterbox/setup-ffmpeg.sh -m <minibox>   # the audio decoders; without them the core is silent
./waterbox/build-package.sh -m <minibox> -r <chimera>
./waterbox/build-native.sh                 # for the gate: the native reference
./waterbox/build-difftest.sh               # for the gate: the CPU harness
```

`build-package.sh` runs `setup-mesa.sh` (fetches a pinned Mesa tarball),
`build-guest.sh` (applies the patches, builds the emulator for the guest) and
`build-core.sh` (links `waterbox/bin/core.wbx`, builds `waterbox/bin/run-wbx`)
and writes `<chimera>/build/Cores/eka2l1.chimeraCore`. It does not run
`setup-ffmpeg.sh`: run that first. Pass `-m` and `-r`: without them the
scripts fall back to `$HOME/chimera`.

## Install the core into Chimera

Chimera ships no cores and downloads nothing: a package is put in its `Cores`
folder by hand. In a source checkout `build-package.sh -r <chimera>` already
wrote it there, as `<chimera>/build/Cores/eka2l1.chimeraCore`. For a release
bundle, copy the file into the `Cores` folder beside `Chimera.exe`, or into
the folder chosen in File > Core Manager > Change folder... File > Core
Manager lists the folder; Refresh List rescans it.

A hand-built package is stamped `<commit>+local` (`-dirty` when the tree has
changes) and is for testing. CI stamps the commit and publishes the `dev` and
`nightly-YYYY-MM-DD` releases.

## Test before you commit

```sh
./waterbox/run-gate.sh
# also, with Chimera's natives built, when the package or its declarations changed:
cd <chimera> && CHIMERA_CORES_DIR=<chimera>/build/Cores dotnet test source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj -c Release --nologo --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests"
```

`run-gate.sh` takes no options and ends with `totals: N pass, N fail`. There
must be 0 failures and exit status 0.

- Without content these legs run and must pass: `upstream`, `cpu`,
  `reference`, `clock`, `control`, `threads`, `sandbox`, `clock-start`,
  `colour`. `SKIP cpu` or `SKIP sandbox` means something was not built.
- `SKIP device` (no `tests/roms-local/SYM.ROM`) and `SKIP ngage2` (no
  `tests/roms-local/ngage2/` set) are expected on a machine without the
  user's ROM and games. Every leg that boots a phone or runs a game is behind
  one of them. `docs/BUILDING.md` lists exactly which file each leg wants.
- Never report a skipped leg as passed. If a change touches what only those
  legs prove (a device, a game, the picture, savestates), say they did not run.

## Rules of this repository

- Never commit inside `extern/eka2l1`. A change to upstream is a numbered
  file in `patches/`, applied at the start of every build; each is a build
  option or a hook, never a deletion. `git status` shows `extern/eka2l1` as
  modified once the series is applied. That is normal: do not stage it, reset
  it or clean it to tidy up.
- The series is judged as a whole: an edit in `extern/eka2l1` that is not in a
  patch stops the next build as "partly patched". To add a patch, edit the
  patched tree, write the change against the tree as the earlier patches
  leave it (a plain `git diff` is against HEAD) as the next numbered file,
  then see `waterbox/apply-patches.sh` print `already applied`.
- Determinism is the product. The machine's clock is virtual and bought with
  executed instructions. The guest must not read host time, host randomness
  or anything else that differs between runs, and a savestate must
  round-trip. The gate checks it; a change that breaks it is a bug.
- The native reference and the sandbox must be the same machine: both build
  from the same options (`configure-flags.sh`) and the same FFmpeg source.
- Run the gate before committing. A new leg needs a negative control: break
  the thing it checks, see the leg fail, and say so in the commit message.
  The `control` leg is the model. Read Chimera's `docs/gates.md` first.
- Never commit a ROM, a device dump, firmware or a game.
- Never add network access. A machine with no network refuses a socket.
- CI runs the shell scripts directly. Keep them executable (git mode 100755).
  `configure-flags.sh` is sourced and is not.
- A script that needs miniBox takes `-m <dir>`, and `build-package.sh` passes
  it down. Keep it so: CI's Chimera checkout is not at `$HOME/chimera`.
- Documentation prose is plain ASCII.
- Commit messages follow the log: `type(scope): a sentence saying what is now
  true`, for example `fix(screen): the 12-bit display mode has blue in the
  high nibble`. Types in use: `feat`, `fix`, `build`, `docs`. A Chimera issue
  is cited as `(chimera#N)`. The body says what changed, why, what was
  measured, the gate's tally and the negative control.
- Do not edit `.github/workflows` unless the task is the workflow.
- Problems with this core are reported in Chimera's issue tracker, not here.

## Where to read more

- `docs/BUILDING.md` - requirements, every script's options, which gate leg
  needs which file, how to add a patch, troubleshooting.
- `docs/PLAN.md` - the milestones and the reasoning behind every decision.
- In the Chimera checkout: `README.md` ("Building"),
  `docs/porting-a-core.md`, `docs/core-manager.md`, `docs/gates.md`.
