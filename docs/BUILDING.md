# Building the EKA2L1 core

This repository builds EKA2L1 (Symbian OS and the Nokia N-Gage) as a sandboxed
guest for [Chimera](https://github.com/ToolAssisted-run/chimera) and packs it
into one file, `eka2l1.chimeraCore`. The steps below are the ones
`.github/workflows/chimera.yml` runs on a fresh clone on a public runner; where
the workflow uses a GitHub Action, the manual equivalent is given. Cores are
built on Linux. The same package file works on Linux and on Windows, because
the guest inside it is run by Chimera's sandbox (miniBox) on either.

Placeholders used below:

- `<core>` - the checkout of this repository.
- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<minibox>` - `<chimera>/extern/chimera-common-minibox`, a git submodule of
  Chimera: the sandbox host and the guest toolchain.

Commands are run from `<core>` unless a `cd` says otherwise.

## Requirements

**Operating system.** Linux, x86-64. CI runs on GitHub's `ubuntu-latest`.

**apt packages.** The workflow's one job installs:

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 mono-complete \
  libgl1-mesa-dev libegl-dev libx11-dev libxext-dev libasound2-dev \
  libsdl2-dev libwayland-dev \
  python3-mako bison flex
```

What the less obvious ones are for:

- `libsdl2-dev libwayland-dev` and the X11, EGL and GL packages - the native
  reference is built the way a normal EKA2L1 is, host devices and all, so
  upstream's drivers ask for them even though nothing here opens a window. The
  guest build turns `EKA2L1_HOST_DEVICES` off and needs none of them.
- `python3-mako bison flex` - this core's own Mesa is a meson build and
  generates its sources with them.
- `mono-complete` and .NET - Chimera's contract tests.

**Compilers.** The workflow pins no compiler version: it uses the runner's
`gcc` and `g++`. `waterbox/guest-toolchain.cmake` takes `gcc-13` and `g++-13`
for the guest when they exist, and plain `gcc` and `g++` otherwise. The reason
is in that file: GCC 14 makes an implicit function declaration an error, and
upstream's libarchive has one against the guest's C library.

**.NET SDK 8.0.** The workflow uses `actions/setup-dotnet@v4` with
`dotnet-version: '8.0'`. Chimera's README gives the manual install:

```sh
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0
```

This core has no Rust.

**What the scripts fetch or build themselves.**

- `waterbox/setup-mesa.sh` downloads Mesa 24.0.9
  (`https://archive.mesa3d.org/mesa-24.0.9.tar.xz`) with `curl` into
  `build/deps/`, checks its SHA256, unpacks it into `build/mesa/`, applies the
  five patches in `patches/mesa/` and cross-builds it for the guest.
  `MESA_TARBALL=<path>` uses a tarball already on the machine.
- `waterbox/setup-ffmpeg.sh` downloads nothing. It compiles the FFmpeg source
  the `extern/eka2l1` submodule already carries
  (`extern/eka2l1/src/external/ffmpeg`).
- The libraries EKA2L1 vendors are built from source by its own CMake, for
  both flavors.

## Get the sources

This repository, with its submodules (`actions/checkout@v6` with
`submodules: recursive`):

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-eka2l1 <core>
cd <core>
git submodule update --init --recursive
```

Recursive is required. `extern/eka2l1` is upstream EKA2L1, pinned by commit,
and it has many submodules of its own under `src/external`. Upstream's CMake
adds them by name, so a one-level checkout stops at configure time with
`does not contain a CMakeLists.txt file` errors.

Chimera (`actions/checkout@v6` of `ToolAssisted-run/chimera` at `main`, then
the workflow's own submodule step):

```sh
git clone https://github.com/ToolAssisted-run/chimera <chimera>
cd <chimera>
git submodule update --init --recursive extern
```

Where the scripts look for these:

- `waterbox/build-package.sh` takes the Chimera checkout from `-r`. Without
  it, it tries `<core>/../chimera`, then `$HOME/chimera`. It takes miniBox from
  `-m` or `MINIBOX_DIR`, else `<chimera>/extern/chimera-common-minibox`.
- `setup-mesa.sh`, `setup-ffmpeg.sh`, `build-guest.sh` and `build-core.sh`
  take miniBox from `-m` or `MINIBOX_DIR`, and fall back to
  `$HOME/chimera/extern/chimera-common-minibox`. Pass `-m` unless Chimera
  really is at `$HOME/chimera`.

## Build miniBox

CI first builds Chimera's native libraries, because the contract tests open
the package through the engine and fail without them:

```sh
cd <chimera>
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
```

Then miniBox, the sandbox host and the guest toolchain:

```sh
mb=<minibox>
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

- `build/meson-linux` holds the sandbox host
  (`source/host/libminiboxhost.so`), which `run-wbx` links.
- `build/meson-cpp` holds the C++ guest toolchain: `guest-sysroot/` with the
  `musl-gcc` specs and libstdc++, and the guest glue objects the core is
  linked with.

CI caches those two directories with `actions/cache@v4`, keyed on the miniBox
commit and its `meson.build`.

## Build the core

The workflow's order:

```sh
./waterbox/build-native.sh
./waterbox/build-difftest.sh
sh waterbox/setup-mesa.sh -m <minibox>
sh waterbox/setup-ffmpeg.sh -m <minibox>
./waterbox/build-guest.sh -m <minibox>
sh waterbox/build-core.sh -m <minibox>
```

### The patches

Upstream is not modified in git. The changes this core needs are the numbered
files in `patches/`, applied to the working tree of `extern/eka2l1` by
`waterbox/apply-patches.sh`. `build-native.sh`, `build-difftest.sh` and
`build-guest.sh` each run it first. After a build `git status` shows
`extern/eka2l1` as modified. That is the applied series, and it is normal.
Each patch is a build option or a hook, never a deletion.

What the script does, in order:

1. It checks that `extern/eka2l1` is a checked-out repository of its own. If
   not, it prints the `git submodule update` command and exits 1.
2. It lists every file the series touches and copies those files, as the
   submodule's HEAD has them, into a scratch directory.
3. It applies every patch, in order, to the scratch copy with `git apply`. If
   one does not apply it stops with `the series does not apply to the
   submodule's HEAD at <patch>`: the submodule was moved without rebasing the
   patches. The real tree is not touched.
4. If the working tree is pristine (every touched file equals HEAD), it applies
   each patch to the tree and prints `applied: <patch>`.
5. Otherwise the tree must be exactly what the whole series leaves behind. If
   it is, the script prints `already applied: all N patches` and exits 0.
6. If it is neither, it names each file that differs, says
   `extern/eka2l1 is partly patched`, and exits 1. It applies nothing.

To start again from the submodule's HEAD (this discards edits made in the
tree):

```sh
git -C extern/eka2l1 reset --hard && git -C extern/eka2l1 clean -fd && waterbox/apply-patches.sh
```

`EKA2L1_TREE=<dir>` points the script at another checkout, for testing the
script itself.

To add a patch: start from a fully patched tree, edit the files under
`extern/eka2l1` without committing there, and write the change as the next
numbered file in `patches/`. It is a unified diff with `--- a/<path>` and
`+++ b/<path>` headers, paths relative to the submodule root, as `git apply`
reads them. It must hold only the new change, measured against the tree as the
earlier patches leave it, not against HEAD: a plain `git diff` in the
submodule would also contain every earlier patch to the same file. Chimera's
`docs/porting-a-core.md` describes the trap: snapshot such a file before
editing and `diff -u` against the snapshot. Then run
`waterbox/apply-patches.sh`: with the new file in `patches/` and the edit
still in the tree it must print `already applied: all N patches` with N one
higher.

`patches/mesa/` is a different series. Those five patches belong to the Mesa
tarball and are applied by `waterbox/setup-mesa.sh` with `patch -p0`.

### The native reference and upstream's suite

```sh
./waterbox/build-native.sh
```

It applies the patches, builds a host FFmpeg (`setup-ffmpeg.sh -n`, into
`build/ffmpeg-native/stage`), configures `waterbox/CMakeLists.txt` with the
option set in `waterbox/configure-flags.sh` into `build/native`, and builds
the ninja targets `run-native`, `install-device` and `ekatests`. Extra
arguments are passed to ninja as further targets.

- `build/native/run-native` - the reference: the emulator and a harness on the
  host, no sandbox. The gate compares the sandboxed core with it. It is also
  where debugging is done.
- `build/native/install-device` - turns a ROM dump into an installed device
  for the reference.
- `build/native/eka2l1/src/tests/ekatests` - upstream's own test suite.

### The CPU differential harness

```sh
./waterbox/build-difftest.sh
```

It builds upstream's `dyncom_difftest` in its own tree, `build/difftest`. The
tree is separate on purpose: building the harness changes the cpu target (a
test-only VFP selector), and the machine this core ships must not be built
that way. The gate runs the binary.

### The guest Mesa

```sh
sh waterbox/setup-mesa.sh -m <minibox>
```

Mesa's softpipe driver behind OSMesa, for the guest. A game that draws through
the Symbian window server is composed with it, inside the core.

- Options: `-m <miniBox dir>`, `-j N`. Environment: `MESA_TARBALL=<path>`;
  `CHIMERA_DEPS_DIR` moves the download cache.
- Output: `build/mesa/build-guest/`. A second run prints
  `mesa: already built` and exits.
- Mesa's own last step, linking a shared `libOSMesa`, fails for a guest. That
  is expected. Only a missing archive or a missing osmesa target object is a
  real failure, and the script checks for both.

Without it the core still links, and draws only what a game paints into the
phone's panel itself.

### The guest FFmpeg

```sh
sh waterbox/setup-ffmpeg.sh -m <minibox>
```

The audio decoders, compiled from the submodule's FFmpeg source for the guest,
with no assembly, no runtime CPU detection and no threads, so they decode the
same samples on every machine.

- Options: `-m <miniBox dir>`, `-j N`, and `-n` for the host flavour that
  `build-native.sh` uses.
- Output: `build/ffmpeg-guest/stage` (guest), `build/ffmpeg-native/stage`
  (host). The configure step is skipped when `config.h` is already there.

Without it the core still builds and runs every game, and none of them makes a
sound.

### The guest emulator, then the core

```sh
./waterbox/build-guest.sh -m <minibox>
sh waterbox/build-core.sh -m <minibox>
```

`build-guest.sh` applies the patches and builds the emulator's libraries with
the guest toolchain (`waterbox/guest-toolchain.cmake`) into `build/guest`:
the same CMake and the same options as the native reference, with no host
devices and no logging. Its option is `-m <miniBox dir>`; further arguments
are extra ninja targets. When `build/ffmpeg-guest/stage/lib/libavcodec.a` is
missing it says `building a silent core` and carries on.

`build-core.sh` compiles the adapter (`make -f waterbox/guest.mk`, objects in
`waterbox/obj-guest/`), links `waterbox/bin/core.wbx` from it, the guest
archives, the guest Mesa and the guest FFmpeg, strips its debug information,
runs miniBox's `check-wbx.sh` on it, and builds `waterbox/bin/run-wbx`, the
runner that loads `core.wbx` through the miniBox host without the frontend.

- Options: `-m <miniBox dir>`, `-o <output dir>` (default `waterbox/bin`),
  `-j N`.
- Environment: `MESA_GUEST_DIR`, `EKA2L1_FFMPEG_GUEST_ROOT` and
  `MINIBOX_HOST_DIR` name a guest Mesa, a guest FFmpeg stage and a miniBox
  host directory built elsewhere.

## Build the package

```sh
./waterbox/build-package.sh -m <minibox> -r <chimera>
```

Options: `-m <miniBox dir>` (or `MINIBOX_DIR`) and `-r <chimera root>`. There
is no `-o`: the output location follows `-r`, and the script packs
`waterbox/bin/core.wbx`.

What it does:

1. Runs `setup-mesa.sh`, `build-guest.sh` and `build-core.sh`. It does not run
   `setup-ffmpeg.sh`, `build-native.sh` or `build-difftest.sh`. Run
   `setup-ffmpeg.sh -m <minibox>` before it, as CI does, or the package is the
   silent core.
2. Stages `core.wbx`, `waterbox.config`, `default_keybinds.json` and
   `file_slots.json` in `build/package-staging/`, with the licence texts that
   miniBox's `package-licenses.py` gathers from
   `waterbox/package-licenses.json`.
3. Stamps the version into the staged `waterbox.config` and writes
   `build.json`, which records what built the package.
4. Zips the staging directory deterministically, twice, and fails if the two
   SHA1s differ. It prints `package sha1 <hash>`.
5. Writes `<chimera>/build/Cores/eka2l1.chimeraCore`, removes any
   `<chimera>/build/CoreCache/eka2l1-*` directory, and prints
   `packaged -> <path>`.

**The version stamp.** A package's version is the commit it was built from. CI
sets `CORE_VERSION` to the commit (`${{ github.sha }}`) for this step. Without
`CORE_VERSION` the script stamps `<commit>+local`, where `<commit>` is the
12-character short hash, and `<commit>-dirty+local` when `git diff --quiet
HEAD` reports changes. The applied patches count as a change to
`extern/eka2l1`, so a hand build normally carries `-dirty`. The commit's date,
in UTC, is stamped beside it as `versionDate`. Hand-built packages are for
testing: Chimera's publishing script refuses a version that carries `+local` or
`-dirty`.

**Releases.** On every green push to `main` the workflow's `publish` job
replaces the rolling `dev` release. The scheduled run (cron `0 4 * * *`)
publishes a dated `nightly-YYYY-MM-DD` release, only when `main` moved since
the last one. Nothing is published from a pull request.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A user
downloads a core's `.chimeraCore` package from the core repository's Releases
page, or builds it, and puts it in Chimera's `Cores` folder.

- In a Chimera source checkout the cores folder is `<chimera>/build/Cores/`.
  `waterbox/build-package.sh -r <chimera>` writes the package straight there,
  so there is nothing more to do.
- In a release bundle it is the `Cores` folder beside `Chimera.exe`, or another
  folder chosen in File > Core Manager > Change folder... Copy
  `eka2l1.chimeraCore` into it.
- File > Core Manager lists what is in that folder. Refresh List rescans it.

Published packages are at
https://github.com/ToolAssisted-run/chimera-core-eka2l1/releases.

## Run the gates

### The gate

```sh
./waterbox/run-gate.sh
```

It takes no options. It needs `build/native/run-native` and `ekatests` and
exits 1 without them. It wipes and uses `waterbox/tests/work/`. Each leg
prints one line that starts with `PASS`, `FAIL` or `SKIP` and the leg's name.
The last line is `totals: N pass, N fail` (SKIPs are not counted), and the
script exits non-zero if anything failed.

**Legs that need no content.** These are what CI runs.

- `upstream` - upstream's own test suite passes in the headless configuration
  this port builds.
- `cpu` - upstream's differential harness: the interpreter this core runs on
  agrees with a golden ALU model and with dynarmic. SKIP when
  `dyncom_difftest` is not built.
- `reference` - the machine says the same thing twice.
- `clock` - with the host stalled five milliseconds every frame, the machine
  does not notice.
- `control` - the same machine left on the host clock does notice the stall.
  This is the negative control for `clock`.
- `threads` - the machine runs on one host thread.
- `sandbox` - `core.wbx` runs the same workload and native equals sandbox.
- `clock-start` - the `clockStart` setting moves the phone's date and nothing
  else, and a bad date is refused.
- `colour` - 12-bit pixels unpack to the colours they name.

`sandbox` and `clock-start` need `waterbox/bin/core.wbx` and
`waterbox/bin/run-wbx`. Without them one line, `SKIP sandbox`, stands for both.

**Legs that need a phone ROM.** The ROM is the user's to supply, as
`tests/roms-local/SYM.ROM`. Without it (or without
`build/native/install-device`) the gate prints one line, `SKIP device`, and
none of the legs below runs or prints anything. That is the case in CI.

- `device` - the ROM installs as a device.
- `boot` - the machine's own menu runs the same instructions every time, also
  with the host stalled.
- `gpu` - the reference composes a screen on a real OpenGL context, the same
  one twice. SKIP when the machine has no OpenGL context.
- `in-box` - native equals sandbox with the device. SKIP when `core.wbx` is
  not built, and then nothing below runs either.
- `state` - a save and reload before every frame changes nothing.
- `install` - a package installs into the machine, native equals sandbox. The
  package is upstream's own test package from the submodule, so this leg needs
  the ROM and nothing else.

**Legs that need the ROM and a game.** Each looks in `tests/roms-local/` for
the file the gate was written against:

- `compositor` - needs one particular N-Gage game archive, by its exact file
  name (the `wserv=` line of that leg in `waterbox/run-gate.sh`). SKIP without
  that exact file.
- `blz` - needs the `.blz` the script names, or failing that the first
  `*.blz` in the folder, and also `BLZinstapp.sis`. SKIP without both.
- `card` - needs the archive the script names, or failing that the first
  `*.rar` or `*.zip` in the folder. SKIP without one.
- `bus`, `keypad`, `session` - run on the same game as `card`, and only when
  it is there. Without it they print nothing; `SKIP card` stands for all four.

**Legs that need an EKA2 phone and an N-Gage 2.0 game.** They need the
directory `tests/roms-local/ngage2/` holding `SYM.ROM`, `SYM.RPKG`,
`ngage.sis` and one `*.n-gage` file, and they need `core.wbx`. Without all
four files the gate prints one line, `SKIP ngage2`. That is the case in CI.

- `ngage2` - the N-Gage application installs and starts the game; two sandbox
  runs are the same machine; the native reference draws the same picture. The
  instruction count is not compared between native and sandbox here: they
  differ, and `docs/PLAN.md` records that as open.
- `ngage2 session` - a state saved half way and loaded in another process
  finishes as the whole run does.
- `setting screenBufferSync=on` and `setting openglEs=software` - each setting
  reaches the machine, two runs agree, and the native reference draws the same
  picture.

The last three run only after `ngage2` passes.

### Chimera's contract tests

This core's frontend check: Chimera's own tests against the package just
built. It must be readable, built for an ABI this frontend runs, become a
working factory, bind only buttons its controller declares, and stamp a
version. They need Chimera's native libraries built (above), and no ROM.

```sh
cd <chimera>
CHIMERA_CORES_DIR=<chimera>/build/Cores dotnet test source/gui/Chimera.Tests.Client.Common/Chimera.Tests.Client.Common.csproj -c Release --nologo --filter "FullyQualifiedName~InstalledCorePackagesTests|FullyQualifiedName~MnemonicUniquenessTests"
```

## Files the core needs at run time

No ROM, device dump or game is in this repository or in the package. The user
provides them. From `waterbox/file_slots.json` and the `firmware` list in
`waterbox/waterbox.config`:

**The project's file is the game.** One file: an N-Gage 2.0 game (`.n-gage`),
an N-Gage card image (`.blz`), a card dump (`.rar`, `.zip`, `.7z`) or a
Symbian package (`.sis`, `.sisx`).

**The phone is firmware.**

| firmware id | file | needed |
| --- | --- | --- |
| `sym.rom` | `SYM.ROM`, a dump of the phone's own ROM. Any Symbian phone's dump is accepted; nothing is pinned. | always |
| `sym.rpkg` | `SYM.RPKG`, the rest of an EKA2 phone's drive Z. It must be the RPKG of the ROM given with it. | for a `.n-gage` game |
| `ngage.sis` | `N-Gage INSTALLER v.1.40.1557.sis`, the N-Gage 2.0 application. 9627728 bytes, SHA1 `D5F580220CE038172C980BE3BD658E0E48A0E882`. | for a `.n-gage` game |
| `blzinstapp.sis` | `BLZinstapp.sis`, the Symbian application that unpacks a `.blz`. 6965 bytes, SHA1 `2DA9BD5ADFE6ACDE66A41D880D85DF3BBE242E64`. | for a `.blz` game |

## Troubleshooting

- CMake stops with `does not contain a CMakeLists.txt file` - the clone was
  not recursive. Run `git submodule update --init --recursive`.
- `miniBox C++ guest toolchain missing at ...`, or
  `miniBox guest sysroot not at ... - pass build-guest.sh -m <miniBox dir>`,
  or gcc saying it cannot read the specs file - the script is looking in
  `$HOME/chimera`. Pass `-m`, and build miniBox's `build/meson-cpp`.
- `no guest FFmpeg at build/ffmpeg-guest: building a silent core.` - run
  `waterbox/setup-ffmpeg.sh -m <minibox>`, then `build-guest.sh` and
  `build-core.sh` again.
- `no guest Mesa at ... - run waterbox/setup-mesa.sh` - without it the core
  draws only what a game paints into the panel itself.
- `build/guest missing - run build-guest.sh first.` - `build-core.sh` only
  links.
- A guest build stops in libarchive on an implicit declaration of
  `arc4random_buf` - the compiler is GCC 14, which makes that an error.
  Install `gcc-13` and `g++-13`; the toolchain file takes them when they
  exist.
- `mesa: <library> was not built - the build really did fail` - unlike the
  expected `libOSMesa` link failure, this one is real. The script hides
  ninja's output; run ninja in `build/mesa/build-guest` to see the error.
- `mesa: the tarball is not the one this core is pinned to` - the file in
  `build/deps/` (or `MESA_TARBALL`) is not Mesa 24.0.9 with the pinned SHA256.
- `ekatests not built` or `run-native not built` from the gate - run
  `./waterbox/build-native.sh`.
- The contract tests fail to load a native library - Chimera's natives are not
  built. See "Build miniBox".
- `extern/eka2l1 is not checked out` or `is partly patched` - see "The
  patches" above.
