# chimera-core-eka2l1

Symbian OS and the Nokia N-Gage as a
[Chimera](https://github.com/ToolAssisted-run/chimera) waterbox core, built from
[EKA2L1](https://github.com/EKA2L1/EKA2L1). The machine runs deterministically
inside the miniBox sandbox: the emulator's clock is virtual and driven by
executed instructions rather than by the wall, savestates are arena snapshots,
and the picture comes from the graphics driver's own command list, executed on
the emulation thread.

- `extern/eka2l1` - upstream, pinned, unmodified
- `patches/` - the local patch series (numbered, applied by
  `waterbox/apply-patches.sh`; each a build option or a hook, never a deletion)
- `waterbox/` - the adapter, the build and the gate
- `docs/PLAN.md` - milestones and the decisions behind them

Upstream is GPL-3.0-or-later; the glue in this repository is MIT. A Symbian
device dump and the games are supplied by the user and live outside the
repository; nothing under `tests/content-local/` (gitignored) is ever committed.
