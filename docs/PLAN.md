# EKA2L1 -> Chimera waterbox core: the plan

Written 2026-09-05 at the start of the effort; update as milestones land. The goal is a
working, deterministic, waterboxed EKA2L1 core package: Symbian OS and the N-Gage as a
citable machine. The dyncom interpreter first, dynarmic as a later speed setting, the
graphics command list executed inline on the emulation thread, no user interface, no
networking, no real audio or input devices.

## What the survey found (2026-09-05, upstream @ 1bc5c8cf2)

- **Scale**: 250k lines under `src/emu`, 502 translation units, plus 40 vendored
  submodules. Roughly flycast's size; a quarter of rpcs3's.
- **Language**: C++17, CMake, exceptions on. Host gcc 13.3 builds it.
- **The core is already a library.** `system` is constructed with an injected
  `drivers::graphics_driver *` and `drivers::audio_driver *`
  (`src/emu/system/include/system/epoc.h:111`), and the frontend's emulation thread is
  a bare `while (!quit) symsys->loop();` (`src/emu/qt/src/thread.cpp:313`). The Qt
  frontend is added unconditionally on desktop (`src/emu/CMakeLists.txt:62`) - that is
  patch 0001, an option, not a deletion.
- **No fastmem, no signal handlers.** Memory is a page-table MMU with an explicit
  address-space model (`src/emu/mem/`). The whole emulator has three `mmap` call sites
  and zero `SIGSEGV` handlers, so the sandbox needs to serve nothing but ordinary
  anonymous pages. This is the single biggest difference from PCSX2 and rpcs3.
- **Time is the wall clock, and it has exactly one seam.** The survey expected two -
  `common::make_teletimer` for the emulated tick source, and the calendar reads for the
  RTC - but the only teletimer implementation, `basic_teletimer_micro`, measures with
  `get_current_utc_time_in_microseconds_since_epoch` itself. So the kernel's own tick
  source, every timer deadline, the kernel's base time, window-server event timestamps,
  package install timestamps, DRM and centralrepo all reduce to one function
  (`src/emu/common/src/time.cpp:45`). Give that function a source and the machine's
  clock is the machine's (M1, patch 0003).
- **`ntimer` owns a host thread** (`src/emu/kernel/src/timing.cpp:86`) that sleeps on
  real microseconds and fires kernel timer events. It already exposes `advance()`
  returning the microseconds until the next event (the comment says nanoseconds and is
  wrong), so the loop can pump it; the thread is what has to go (M1, patch 0004).
  `reset()` is also what starts the timer's measurement - a timer that never had it
  reads the same instant forever and nothing it holds ever comes due.
- **An empty machine cannot be stepped.** Without a device there is no memory model, and
  `thread_scheduler::switch_context` dereferences it while switching to the thread that
  cannot exist (`src/emu/kernel/src/scheduler.cpp:89`). Time still passes for such a
  machine and its timers still come due, which is what M1 tests with.
- **The scheduler blocks when nothing is runnable** (`src/emu/kernel/src/scheduler.cpp:131`),
  but only behind `kernel_system::should_core_idle_when_inactive()`, which is the
  `cpu_load_save` config flag (`src/emu/kernel/src/kernel.cpp:1462`). Turned off, the
  loop returns instead of sleeping and the driver jumps virtual time to the next timer
  deadline. No patch needed for this one.
- **The logger writes two files into the working directory**, `EKA2L1.log` and
  `EKA2L1_TakeThis.log` (`src/emu/common/src/log.cpp:200`), with no way to point it
  elsewhere. Harmless natively, but a core does not write where it likes: inside the
  sandbox this lands in the memfs, and the sink becomes a seam of its own at M2. The
  configuration writes itself back to `config.yml` in the working directory too.
- **Other host threads, all removable**: the FBS bitmap compressor
  (`src/emu/services/src/fbs/fbs.cpp:461`), the applist loading pool
  (`src/emu/services/src/applist/applist.cpp:149`), ffmpeg's video decode thread, the
  SDL2 controller poller, the filesystem watcher and upnp. None of them is on the path
  of a game that draws and plays sound; each becomes synchronous or is compiled out.
- **CPU: three backends, one of them an interpreter.** `dyncom` (interpreter),
  `dynarmic` (x86-64 JIT), `12l1r` (their own recompiler, 32-bit ARM hosts only).
  Upstream already carries `EKA2L1_CPU_DYNCOM_ONLY_BUILD` and a differential harness
  that checks two backends agree (`src/emu/cpu/src/arm_factory.cpp:32`). The
  interpreter is the reference; dynarmic is a later setting with an agreement leg.
- **Graphics is a command list, not a live GL API.** Everything the OS side does goes
  through `submit_command_list` (`src/emu/drivers/include/drivers/graphics/graphics.h:185`)
  and is executed by `shared_graphics_driver` on the frontend's graphics thread. On our
  side the queue is pumped inline after each slice, so there is no second thread. The
  backend is glad-loaded OpenGL / GLES3 (`.../backend/ogl/graphics_ogl.cpp:36,58`), which
  is the same shape the GL bridge already feeds. `graphics_driver_read_bitmap`
  (`src/emu/drivers/src/itc.cpp:220`) is the video readback, already a supported command.
- **Audio is a pull-callback stream**: `audio_driver::new_output_stream(rate, channels,
  callback)` (`src/emu/drivers/include/drivers/audio/audio.h:75`). Our driver renders
  exactly the samples a frame needs and never touches a host device.
- **The N-Gage ROM is a device on its own.** The user's `SYM.ROM` installs through
  `install_rom_with_optional_rpkg` with no RPKG needed and comes up as
  `(c)NMP V 4.03 (NEM-4)`, epocver 2 (Symbian 6.1, EKA1) - the N-Gage classic. It
  carries 58 applications on drive Z.
- **`.blz` is not a game EKA2L1 can read.** It is a Blizzard installer package, the
  format N-Gage titles were distributed in, and upstream has no code for it at all. The
  supported route is a third-party `BLZinstapp` SIS installed into the machine, the
  `.blz` placed on drive E, and the installer run inside the emulator to unpack it.
  What that leaves behind - an installed application on drive C - is what a core would
  actually cite, which makes it bundle content rather than a ROM.
- **There is no "boot the OS".** EKA2L1 reimplements the kernel and runs individual
  Symbian executables; nothing happens until an application is launched, by
  registration through the application list server or by path. Until then the machine
  runs zero instructions, which is not a fault.
- **A second host thread appears when a Symbian app watches a directory.**
  `io_system::watch_directory` builds a `common::directory_watcher` on first use
  (`src/emu/vfs/src/vfs.cpp:1013`), and that is inotify plus a thread. It has to go for
  the guest; M2's problem.
- **Content is a device dump plus a game.** A device installs from a single archive
  holding `data/drives/z/<firmware code>/` and `data/roms/<firmware code>/`
  (`src/emu/system/include/system/installation/archive.h:39`) - one file, one SHA1, so
  Chimera's firmware channel takes it unchanged. Games are `.sis`/`.sisx` installs or
  N-Gage game cards (`system::install_ngage_game_card`, `epoc.h:183`). Whatever the
  install writes is persistent device state, which is what the bundle mechanism is for.
- **The embedder owes the emulator four host functions.** `drivers::ui::open_input_view`,
  `close_input_view`, `show_yes_no_dialog` (`src/emu/drivers/include/drivers/ui/input_dialog.h`)
  and `common::launch_browser` are declared, called from the dispatcher and the notifier
  service, and defined only by the frontends. A core answers all four itself. The yes/no
  answer cannot be given where the question is asked - the notifier holds the kernel
  lock, and completing the request takes that same non-recursive lock - so it is queued
  and delivered between steps (`waterbox/host-ui.cpp`).
- **Upstream ships its own Symbian test app** under `src/intests/` with expected
  outputs, and a host-side gtest/Catch2 suite (`src/tests/`, target `ekatests`) covering
  the kernel, the MMU, the VFS, the loader, the services and the drivers. The latter
  needs no ROM at all and is the first honest workload this port can run.

## Architecture decisions

- **Upstream pin**: `extern/eka2l1` = EKA2L1/EKA2L1 @ `1bc5c8cf2`, submodules recursive.
  Unmodified; local changes live in `patches/` (numbered, applied by
  `waterbox/apply-patches.sh`), each a build option or a hook, never a deletion.
- **Upstream's own CMake, driven by our option set.** `waterbox/build-native.sh` and
  `waterbox/build-guest.sh` configure the same tree with the same options and differ
  only by the toolchain file, the dolphin recipe. Off: the Qt frontend, the tools, the
  Vulkan backend, LuaJIT scripting, discord, upnp, the camera. The vendored externals
  we cannot avoid (ffmpeg, capstone, mbedtls, freetype, libarchive, xz, zlib, re2,
  lunasvg, libtess2, pugixml, yaml-cpp, spdlog, fmt, glm, stb, xxHash, libfat) are built
  from source for both flavors, so decoded media and packed bitmaps are flavor-identical.
- **Virtual time is the core's, not miniBox's.** A `chimera_teletimer` returns
  microseconds derived from instructions retired, `ntimer::advance()` is pumped from the
  driver's step loop instead of a thread, and the calendar clock is a fixed epoch plus
  the same virtual offset. The native reference build uses it too, so native and sandbox
  are the same machine and the equivalence gate means something.
- **A frame is a fixed slice of virtual time.** Symbian has no video clock: nothing in
  the machine says when a frame ends. The core declares 60 Hz by default and steps the
  machine by one slice of virtual microseconds per `FrameAdvance`, then reads back
  whatever the window server has composited. An `fps` setting (ruffle's precedent)
  raises the rate when a game wants finer input granularity; the movie cites it.
- **The interpreter is the reference.** dyncom for every equivalence and rewind leg;
  dynarmic arrives as a `cpuBackend` setting with its own agreement leg, never as the
  default until it has one.
- **No entropy in the box**: the calendar clock, the RNG, the MMC id and the device
  serial are fixed by the core, not read from the host.

## Milestones

- **M0 - the native reference builds and runs headless. DONE 2026-09-05.** Patch 0001 makes the desktop
  frontend optional; `build-native.sh` configures upstream with the option set above and
  builds the emulator libraries plus `ekatests`. Proof: `ekatests` green, and a
  `run-native` harness that constructs `system`, starts it with no device installed and
  tears it down cleanly, twice, with byte-identical output.
- **M1 - virtual time. DONE 2026-09-05.** One clock seam (0003), the timer driven by its
  owner rather than a thread (0004), the owner told how much CPU ran so it can pay for it
  in time (0005), and the app scan able to run on the calling thread (0006, ordering
  follows the pool otherwise). `cpu_load_save` off and the FBS compressor left disabled,
  both config flags. The glue is `waterbox/vclock.*` (the clock) and `waterbox/machine.*`
  (the machine and its step loop: run the emulator, pump the timer, and buy idle time
  outright by jumping to the next deadline). Proven by a kernel timer every millisecond
  over a second of emulated time: 999 firings, none of them late, identical with the
  host stalled 5 ms every frame - and the same machine left on the host clock does
  notice that stall, which is what says the check has teeth. One host thread.
- **M2 - the machine runs. DONE 2026-09-05, both flavors.** The native half: the ROM installs as a
  device (`waterbox/install-device`, provisioning, outside the machine), drive Z, C, D
  and E mount, the application list scans, and the machine's own menu launches through
  its registration and executes 681,873 ARM instructions in a second of emulated time -
  the same 681,873 every run, and the same with the host stalled 3 ms a frame. The
  sandbox half: the whole emulator - 250k lines and 40 vendored libraries - compiles
  under the miniBox musl toolchain with two more patches, `core.wbx` links and passes
  check-wbx, and it runs the same workload as the native reference to the microsecond.
  What the sandbox still lacks is a filesystem: the device dump cannot reach the machine
  inside the box yet, so what runs there is the empty machine. That is M3's first job.
- **M3 - the device in the box. DONE 2026-09-05.** The guest filesystem is written
  and the device boots inside the sandbox from its ROM and a fifty-seven byte
  descriptor: 58 applications found, the machine's own menu launched, and native ==
  sandbox to the instruction (682,640 of them).
- **M4 - the picture.** The command list pumped inline, the ogl backend fed by the GL
  bridge, `read_bitmap` into the frame buffer. Proof: the composited screen matches the
  native reference's pixels.
- **M5 - input, audio, and a game.** Keys through the N-Gage keypad map, the audio sink,
  a real game card booting to its title screen.
- **M6 - savestates and rewind.** Arena snapshots, the rewind leg, the GPU-state rule
  the bridge already carries.
- **M7 - the package.** `eka2l1.chimeraCore`, the firmware declaration for the device
  dump, the bundle for what the device writes, default keybinds, the licence manifest.
- **M8 - dynarmic.** The JIT as a setting, with the interpreter-agreement leg from
  upstream's own differential harness.

## What the sandbox needed (M2, 2026-09-05)

- **Two more patches.** 0007 (`EKA2L1_HOST_DEVICES`) leaves out the backends that talk
  to host devices - SDL2 input and vibration, the GLX/EGL/Wayland contexts - which the
  iOS branch had already shown the emulator can do without; the two shared sources that
  pick a backend at compile time needed a case for "no platform". 0009 makes the default
  libuv loop lazy: it was a namespace-scope `shared_ptr` built before `main`, so every
  process linking uvlooper opened an epoll descriptor it never used, and a sandbox with
  no epoll could not even start the program to say so.
- **FFmpeg is off for both flavors** (0008). The submodule ships PREBUILT libraries per
  desktop platform, built against the host's C library, which a musl guest cannot link.
  Rather than let native and sandbox decode differently, neither decodes until the guest
  has an FFmpeg of its own - the rpcs3 recipe, when a game asks for sound or video.
- **Logging is compiled out of the guest** (`DISABLE_LOGGING`, upstream's own switch).
  It removes the log file the emulator otherwise writes into the working directory, and
  with it the sink that would have thrown when that write failed.
- **Five libc gaps** in `waterbox/guest-syscalls.cpp`, each found by the sandbox naming
  the syscall it would not serve: `closefrom` (libarchive, before an exec that cannot
  happen), `clock_getres` (libstdc++'s chrono, before anything else runs), `getcwd`, and
  the directory calls the emulator lays its storage root out with (`mkdir`, `mkdirat`,
  `rmdir`, `chdir`, `chmod`, `umask`). The sandbox holds a flat set of mounted files, so
  the directory calls answer success and every name resolves to a mounted file or to
  nothing.

## The guest filesystem (M3, 2026-09-05)

The sandbox mounts a flat list of named files: no directories, no creation, no
enumeration. Symbian needs all three, so the drives are served from
`waterbox/memfs.cpp` - an implementation of EKA2L1's own `abstract_file_system`,
held in the machine's memory and added with `io_system::add_filesystem`. A file in
it is either bytes the machine wrote, which are the machine's state and belong in
its savestates, or a slice of a device pack the host mounted, read as the machine
reads it: fifteen megabytes of ROM files that never change are not state, and
copying them into the arena would put them in every savestate for nothing.
`waterbox/gen-device-pack.py` builds the pack from a storage root
`install-device` wrote; entries are sorted, and nothing about the host reaches it.

What the machine still reads from the host is the ROM itself, mounted under the
name the emulator builds for it (`<storage>/roms//<firmcode>/SYM.ROM`, which
`run-native --print-rom-path` prints rather than anyone guessing).

Six patches and four findings came out of it:

- **0010**: a marker file the emulator could not write took the whole emulator
  down through `fclose(nullptr)`.
- **0011**: `map_rom` maps the ROM file into memory, and a sandbox that serves
  files but has no file-backed mapping cannot. It reads the ROM instead.
- **0012, 0013**: registering the bluetooth stack started libuv's loop - an epoll
  descriptor and a thread - on every machine that booted, and the inet protocol
  took the default loop when it was built rather than when a socket asked.
- **0014**: a drive with no host path cannot be mounted through
  `mount_physical_path`, and nothing announced it, so the application list never
  scanned it.
- **0015**: the window server read `wsini.ini` by host path through
  `get_raw_path`, which a filesystem with no host path cannot answer. It reads
  through the VFS now, which needed `ini_file` and `dynamic_ifile` to accept
  bytes as well as a path.
- `read_file` returns **bytes**, not elements, and a `seek` in `address` mode
  answers `0xFFFFFFFF` for a file that is not in ROM. Both were silent: the first
  made every application registration fail to parse, the second sent the caller
  looking for an icon at an address that was not one.
- Executables on a Symbian 6 device's drive Z are ROM images, not E32 images, and
  the loader picks between them by asking the file whether it is in ROM. Answer
  no and nothing on the machine ever starts.

**The ROM serves its own drive (0016).** EKA2L1 already reads drive Z out of the
ROM image, but every lookup began with "don't bother if it's not even available on
host" - so a device could only be used with a copy of its ROM's files unpacked
beside it, and the copy answered where the ROM should have. The ROM is asked first
now, the host answers only for what the ROM does not have, and `rom_file_system`
enumerates its own directories rather than borrowing the host's. Its tree walker
throws on a path with nothing after the drive, which nothing used to reach because
of that same gate, so the lookups are guarded.

What crosses into the sandbox is therefore the ROM and a **fifty-seven byte
descriptor** naming the device and its Symbian version - no unpacked drive Z at
all. Drive Z is the ROM's; C, D and E are the machine's own memory. Native and
sandbox run the machine's menu to the same 682,640 instructions, and the extracted
copy no longer changes the answer on either side.

## Open questions

- Which device dump and which games the gate will cite. Nothing real can be proven
  without one, and neither dump nor game is ever committed.
- Whether a frame boundary at a fixed slice of virtual time is stable enough for
  rerecording, or whether the window server's composite should define it instead. The
  first is simpler and matches ruffle; the second is truer to what the machine draws.
- Whether the guest needs ffmpeg at all for the first playable games, or whether the
  MMF path can stay stubbed until something asks for it.
