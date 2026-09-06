# chimera-core-eka2l1

Symbian OS and the Nokia N-Gage as a
[Chimera](https://github.com/ToolAssisted-run/chimera) waterbox core, built from
[EKA2L1](https://github.com/EKA2L1/EKA2L1). The whole machine runs
deterministically inside the miniBox sandbox: the emulator's clock is virtual
and bought with executed instructions rather than read off the wall, savestates
are arena snapshots that outlive the process that wrote them, and the core
carries its own OpenGL so that what a game draws does not depend on anybody's
GPU.

## What a project is

**The project's file is the GAME.** The machine it runs on is firmware, because
one phone ROM serves every game:

| | |
| --- | --- |
| game (the project's file) | `.blz`, `.rar`, `.zip`, `.7z`, `.sis`, `.sisx` |
| `sym.rom` (firmware, always) | a dump of the phone's own ROM - it says which device this is and carries drive Z |
| `blzinstapp.sis` (firmware, `.blz` projects only) | the Symbian application that unpacks an N-Gage card image |

A card dump is copied onto the machine's memory card, a package is installed
onto its C drive, and a `.blz` is unpacked by running BLZinstapp inside the
machine and working its menu - all before the first frame, so a movie begins
with the game already running. Everything any of that writes lives in the
machine's own memory, so it travels in every savestate.

No ROM, dump or game is in this repository, and `tests/roms-local/` is
gitignored.

## Two pictures, one machine

Symbian games draw in two ways and the core answers both:

- **Direct screen access** - the game paints the phone's panel itself. The panel
  is machine memory, so the picture is read straight out of it: no graphics
  driver, no GPU, and the sandbox's picture is the native reference's byte for
  byte.
- **Through the window server** - the game records draw commands and something
  else has to compose them. That needs an OpenGL, and a sandbox has no GPU to
  borrow one from, so the core carries **Mesa's softpipe** (`waterbox/gl-osmesa.c`,
  built by `waterbox/setup-mesa.sh`). softpipe is plain C with no JIT and no
  dispatch on host CPU features, so it draws the same picture everywhere.

## Building

    waterbox/setup-mesa.sh          # the machine's own OpenGL (pinned, cached)
    waterbox/build-native.sh        # the reference: emulator + harness, no sandbox
    waterbox/build-guest.sh         # the emulator for the guest toolchain
    waterbox/build-core.sh          # links core.wbx
    waterbox/build-package.sh       # all of the above -> <chimera>/build/Cores/eka2l1.chimeraCore

`waterbox/build-difftest.sh` builds upstream's own CPU differential harness, in
its own tree, for the gate.

## The gate

    waterbox/run-gate.sh

Nineteen legs. Upstream's own test suite; upstream's differential harness over
the interpreter this core runs on; the clock (the machine does not notice a
stalled host, and the check for that has teeth); one thread; then, whenever the
user's own ROM and games are in `tests/roms-local/`, the machine itself - the
device boots, a package installs, a card runs, a `.blz` unpacks itself, the
keypad reaches the game, the window server composes inside the sandbox, memory
reads the same in both flavors, and a state saved by one process is loaded by
another and lands where the uninterrupted run did.

Every one of those compares the **native reference against the sandbox**, which
is the whole claim this core makes: the same machine either way, to the
instruction.

## Layout

- `extern/eka2l1` - upstream, pinned, unmodified
- `patches/` - the local series (numbered, applied all-or-nothing by
  `waterbox/apply-patches.sh`; each one a build option or a hook, never a
  deletion), and `patches/mesa/` for the guest OpenGL
- `waterbox/` - the adapter, the builds and the gate
- `docs/PLAN.md` - the milestones, and the reasoning behind every decision

## Licences

Upstream EKA2L1 is GPL-3.0-or-later and Mesa is MIT; the glue in this repository
is MIT. `waterbox/package-licenses.json` is what the bundle's `LICENSES.md` is
computed from.
