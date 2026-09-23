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
- **M4 - the picture. DONE 2026-09-05.** Red Faction draws its title screen and its
  menu, and the sandbox draws them pixel for pixel identically to the native
  reference with no OpenGL anywhere. Three things were missing, and only the last
  was about drawing at all - see "What the picture needed" below.
- **M4 (was) - the picture.** The command list pumped inline, the ogl backend fed by the GL
  bridge, `read_bitmap` into the frame buffer. Proof: the composited screen matches the
  native reference's pixels.
- **M5 - input and audio. DONE 2026-09-05** (`waterbox/input.*`, `waterbox/audio.*`); a
  game is still not loadable.
- **M5 (was) - input, audio, and a game.** Keys through the N-Gage keypad map, the audio sink,
  a real game card booting to its title screen.
- **M6 - savestates and rewind. DONE 2026-09-05.** It needed no code in the core: the
  whole machine lives in the sandbox's arena, and because the picture is read out of
  the machine's own memory rather than off a GPU there is nothing session-local to
  lose. Two gate legs prove it. `state`: the machine saved and reloaded before EVERY
  frame runs identically to one that was not (60 round trips of an 8.4 MB state; the
  game's is 18.9 MB and 1,500 round trips still match instruction for instruction and
  pixel for pixel). `session`: 600 frames saved in one process, the other 900 run in
  another, and the machine ends where the uninterrupted run did - which is what a
  movie asks of a core, and the leg a core holding a graphics context fails.
- **M7 - the package. DONE 2026-09-05.** `eka2l1.zip` builds and installs into a chimera
  checkout: `waterbox.config` (a Symbian machine, a 176x208 screen at 60 Hz, 44100 Hz
  stereo, a 21-key Series 60 keypad), `file_slots.json` (one slot - the DEVICE ROM, since
  a project is a device until games load), `default_keybinds.json`, and the licence
  manifest (GPL-3.0-or-later; no ROM, dump or game is ever included). The keypad reaches
  the machine through the emulator's own keybind table, whose source numbering is the
  core's button indices; sound is summed from every playing stream into one frame's worth
  at the end of the frame.
- **M8 - dynarmic. MEASURED AND REJECTED 2026-09-05.** The recompiler works and is
  correct - it is upstream's own oracle for the interpreter, and 5,508 whole programs
  agree - but it makes the emulated machine SLOWER, so it is not offered as a setting.
  See "What the recompiler costs" below. What the milestone leaves behind is the
  interpreter-agreement leg, which is worth more: it says the processor this core
  actually runs on is right.

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

What crosses into the sandbox is therefore **the ROM, and nothing else** - no
unpacked drive Z, and since patch 0018 no descriptor either. Drive Z is the ROM's;
C, D and E are the machine's own memory. Native and sandbox run the machine's menu
to the same 682,640 instructions, and an unpacked copy no longer changes the answer
on either side.

**A ROM says which device it is (0018).** `determine_device_from_rom` reads the
same files the folder-based detection reads - `resource/versions/platform.txt` and
`product.txt`, or the older `system/versions/sw.txt` and `model.txt`, and the
`series60v*.sis` names in `system/install` - straight out of the image, using the
memory forms of the INI and text readers that 0015 added. `system::set_rom_path`
then lets the ROM be wherever the embedder put it rather than under a storage
layout nobody built. The user's SYM.ROM answers `(c)NMP V 4.03 (NEM-4)`, epoc6,
which is exactly what unpacking it and inspecting the folder gives.

## The machine on a real OpenGL context (M4, 2026-09-05, unfinished)

Patch 0017 gives the emulator's renderer three things an embedder has to bring:

- **The context is the embedder's.** `graphics::set_gl_context_factory` and
  `set_gl_proc_loader`: a build with no window system has no context backend, and a
  core has a context already, made and current. The glue's `borrowed_gl_context`
  (`waterbox/gl-context.cpp`) is honest about what that means - making it current
  is nothing to do, swapping buffers is nothing to do - and the native reference
  makes the context with EGL's surfaceless platform (`waterbox/gl-egl.c`).
- **The driver runs on the stepping thread.** `driver::pump()` runs what is queued
  and returns, where `run()` is a loop that owns a thread.
- **A driven driver answers its own synchronous commands.** `send_sync_command`
  submits and then waits on a condition the graphics thread signals; with no such
  thread that waits for nobody, so `driver::set_driven(true)` makes it pump
  instead. `read_bitmap` is one of those commands, and reading the screen is the
  whole point.

The path works end to end: the driver is created, the machine's window server
makes its 176x208 screen, the screen is composed on demand
(`scan_for_redraw(..., force)`) and read back through the command list, and two
runs give the same digest.

## What the picture needed (M4, 2026-09-05)

The plumbing above was right from the start. Three other things were missing, and
finding them took the machine apart in this order.

**1. The emulator expects files beside itself, and a sandbox has no beside.**
EKA2L1 opens two sets of files by path at runtime: the shaders its graphics driver
draws with (`resources/*.vert`, `*.frag`), and the patch libraries it uses to
replace ROM routines with its own (`patch/*.map` and the DLLs beside them). Both
were simply absent. The shaders failing is silent in the worst way - the driver
logs a compile error nobody reads, every draw becomes a no-op, and the screen is a
clean, stable, reproducible black. Patches 0023 and 0024 let both come from memory
instead of a folder, and `waterbox/gen-embedded-files.py` compiles them into the
core: 260 KB of patch libraries and 8 KB of shaders, read in a fixed order so every
build gets the same bytes.

**2. The screen driver is one of those patch libraries.** A Symbian game draws
through `CFbsDrawDevice`, from SCDV.DLL. EKA2L1 replaces it (the map file has an
`[epoc6]` section, so it covers this Symbian 6.1 device; the DLL that serves it is
`scdv_v81a.dll`, found by the version-walk fallback) with an implementation whose
`Update()` tells the emulator a frame is ready. Without it the game drew into the
framebuffer and nothing ever looked. With it, the emulator uploads the framebuffer
and 704 frames arrive in fifty seconds of emulated time.

**3. The picture is machine memory, so no OpenGL is needed to read it.** A direct
screen access game writes into the panel's own buffer - a chunk in the machine's
address space. `machine::read_screen` now reads that buffer and converts it (12,
16, 24 and 32 bit panels) rather than composing the window tree and reading the
GPU. It costs nothing, it works in the sandbox where there is no GL at all, and it
makes the sandbox picture identical to the native reference's by construction
rather than by luck. The compositor path stays for everything that is not drawing
directly, and `screen::dsa_active_count()` (patch 0025) says which case this is.

Red Faction now draws its N-Gage splash, its title screen and its menu, and the
gate compares the picture in both flavors: `screen: 176x208 digest
7326d676ffda67e5 lit 34916`, the same on both sides.

**What still does not draw is this ROM's own UI shell.** The window server rejects
a `set_clipping_region` because "Region object's header data size is not 4 bytes" -
an EKA1 command shape its parser does not accept - the AVKON capability server
answers an unimplemented opcode with a fake success, and the menu application stops
at an unimplemented application-list opcode. Individual ROM applications do paint
(the telephone application composes 4,444 lit pixels, the one the gate runs 36,608),
but the S60v1 shell does not come up far enough to be a phone. That is upstream
compatibility with this particular ROM, not anything the core brings, and the
machine it matters for is the game.

## A package installs into the machine (2026-09-05)

Patch 0019 makes the SIS installer write through the emulated filesystem instead
of resolving a host path and writing there. Nothing else changes: the same
interpreter, the same decompression, the same targets - but the path it extracts
to is the machine's (`C:\sys\bin\...`) rather than the host's, so a drive that
has no directory behind it takes an installation like any other. A package inside
a package is the one thing that still wants a host path to identify itself, so a
drive without one simply does not nest.

Proven with upstream's own test package (`src/intests/sis/intests.sis`, in this
repository, needing nobody's ROM): 51 entries and 77,399 bytes land on drive C,
identically in both flavors, and what lands there is the machine's memory - so it
is in every savestate and every movie made from the project.

**A game card loads too, and a real game runs.** The emulator's own card installer
unpacks an archive into a host staging directory and then copies it with host
calls, which a machine whose drives are its own memory has no use for, so the glue
copies the card straight out of the archive onto drive E through the VFS
(`machine::install_card`) and lets the application scan find it. Red Faction - a
commercial N-Gage title, as a RAR of its card - installs, registers as
`0x101fd3d0 RedFaction`, and RUNS: 80,598,663 instructions, identical in both
flavors, reading its own `redfaction.cfl` off drive E.

Two more things the sandbox needed for it: musl's `open()` asks for O_CLOEXEC with
a second, raw `fcntl` call that no libc-level definition can shadow, so the flag is
stripped before the open happens; and the bluetooth teardown path asked whether the
libuv loop had started, which through the lazy proxy BUILT one - an epoll
descriptor and a thread, while tearing down. Asking is not using: the proxy answers
"no" for a loop nobody built (0009), and a machine told it has no network refuses a
socket rather than starting a loop for it (0020).

**The game does not draw yet, and the trail is a real one.** It opens `Lcd.LDD`,
the N-Gage's direct-LCD logical device driver, and then stops in
`wait_for_any_request`. Two things were found behind that:

- The EKA1 way of opening a logical device, `bus_dev_open_socket`, answered "fine"
  and handed back nothing, so every call the caller made on the channel found no
  channel. Patch 0021 opens a real channel from the factory and returns its handle,
  the same way the EKA2 path already did.
- `lcd` itself is still not emulated - EKA2L1 has `videodriver`, `ekeyb`, `dhal`,
  `ecomm`, `gd1drv` and `cameraldd`. Registering the video driver under that name
  was tried: the open then succeeds and the game gets 193 instructions further,
  which is not a device driver, so it was not kept. A machine should not claim to
  have a device it does not have.

Then a second thing, found by asking every thread in the machine what it was
doing: `MediaServer` and `XFcSound` were **dead**. The media server opens
`AUDIO.LDD`, which is not emulated either, and rather than report the failure it
dereferences the channel it did not get - KERN-EXEC 3 - taking itself, its task
thread and the game's sound thread with it. Everything waiting on sound then waits
forever.

Patch 0022 gives those a **null device**: a channel that opens, accepts every
control and request, completes them, and does nothing. That is honest for the sound
hardware, which the audio driver drives rather than the guest's device, and what
matters is only that it opens. With it, no thread dies: the machine keeps
`MediaServer`, `MediaServerTaskThread`, `XFcSound` and a `DSA sync thread` alive,
so the game has set up direct screen access. `lcd` deliberately did NOT get one - a
screen device that answers nothing would be a lie about the screen.

Neither of those was what stopped the picture, as it turned out - see "What the
picture needed". The game never issues a single operation on the `Lcd.LDD` channel
it opens, and registering a null device for `lcd` changes its instruction count by
nothing at all: it opens the driver out of habit and draws through the screen
driver library instead. `lcd` deliberately still has no device.

**The core was perturbing its own machine.** Chasing the divergence this opened up
found it: the guest registered a kernel timer every millisecond, left over from the
milestone that proved the clock, so the shipped core did 999 extra kernel events a
second that the native reference did not. It is the host's to ask for now
(`SetTimerCheckUs`), which is what the gate does, and the two flavors agree again -
74,720,168 instructions each.

`.blz` needs a third-party installer app (BLZinstapp) run inside the machine, which
now has a picture to run it by.

## Open questions

- Which device dump and which games the gate will cite. Nothing real can be proven
  without one, and neither dump nor game is ever committed.
- Whether a frame boundary at a fixed slice of virtual time is stable enough for
  rerecording, or whether the window server's composite should define it instead. The
  first is simpler and matches ruffle; the second is truer to what the machine draws.
- Whether the guest needs ffmpeg at all for the first playable games, or whether the
  MMF path can stay stubbed until something asks for it.

## What the recompiler costs (M8, 2026-09-05)

dynarmic builds for both flavors and runs the game. It is about 2.2x faster in
wall-clock per instruction, and it is deterministic - two runs give the same
digest. It is also correct: upstream's `dyncom_difftest` uses it as the oracle
for the interpreter and 5,508 randomized whole programs agree, with zero
interpreter fallbacks.

And it halves the game's frame rate. Over the same 25 seconds of emulated time,
Red Faction presents **508 frames on the interpreter and 282 on the recompiler**.
The instruction counts say why: 431k instructions per game frame on dyncom,
553k on dynarmic - 28% more work charged for the same frame. A block recompiler
finishes the block it is in rather than the quantum it was given, and every one
of those overshot instructions is charged to a clock that buys time at 484 MIPS.
Pay 28% more for a frame and it lands the wrong side of the vsync the game sleeps
on, so the game waits for the next one: half rate, exactly.

That is not a trade worth offering. The interpreter already runs this machine at
about twenty times real time (1,500 emulated frames in 1.2 seconds), so nothing
is waiting on the processor, and a setting that makes the emulated game run at
half speed while the host works less is a trap rather than an option. It would
also split the core in two: a movie recorded on one backend would not replay on
the other, since the two machines schedule differently.

`run-native --cpu dynarmic` keeps the recompiler reachable for measurement. The
core does not declare it as a setting, and `waterbox/build-difftest.sh` builds
the harness that now gates the interpreter.

## The game starts itself (2026-09-05)

The core had a hole nobody had walked into: nothing started the game. `Init`
booted the phone and installed the project's card, and then the machine sat
there. The gate never saw it because both flavors were told `--run 0x101fd3d0`
by hand, and the frontend has no such button to press: a chimera core is handed
its files and expected to run.

So `Init` starts it. Which application is the project's is not a question the
registration can answer - on this EKA1 device every ROM application claims to
have landed on drive C, whatever drive it is really on - so the machine notes
which applications it had before the project's file arrived and starts the one
that was not there before (lowest UID, so a card holding several always starts
the same one). `LaunchAppUid` still exists for the gate's ROM-application legs,
and answers rather than obeys when asked for the application already running.

The card leg now passes no `--run` at all: the sandbox boots the phone, installs
the card, starts Red Faction and draws it, and the native reference does exactly
the same thing (`launched: 0x101fd3d0`, 219,242,497 instructions, screen
7326d676ffda67e5 on both sides).

## The keypad, and how to ask a machine what it wants (2026-09-05)

With a game finally running, the keypad could be checked against something that
answers. It works, and the mapping written blind in M5 turns out to be right -
but finding that out took a lesson worth keeping.

The first four attempts said input did nothing. All four pressed a key a third
of the way into the run and read the screen at the end. Red Faction's menu goes
back to its attract loop after a few seconds of nobody there, so every run ended
on the same picture whatever had been pressed. **Press near the end and read the
screen straight afterwards**: `--press` in both harnesses now holds a key at
frame N-200 and lets it go at N-180, and the answer is on the last frame.

What actually reaches the game was settled by sweeping every one of the 256 scan
codes straight into the window server, past the keypad map (`--press-raw`). On
the main menu the game answers exactly three of them: 0x10 and 0x11 (up and down
arrows) move the selection, and 0x35 ('5') chooses. The soft keys and the call
key do something too. That is the N-Gage keypad the core declares, so nothing
had to change.

The gate now holds Down on the menu and checks that the game answers - and that
both flavors answer identically (`screen: 176x208 digest e5e24b9c5448a039`).

## End to end, through chimera itself (2026-09-05)

The core was also run the way a user will run it: a hand-written
`.chimeraProject` naming the ROM in the `rom` slot and the game card in the
`game` slot, played headlessly by `chimera-run` against the packaged
`eka2l1.chimeraCore`. It boots, installs, starts the game, draws the title
screen at frame 1499 and the menu at 3000, and a movie that holds Down at 3000
moves the selection. That is the whole path - project, engine, package, sandbox,
picture, input - with nothing of the harness in it.

## Memory for the watch tools (2026-09-06)

A chimera core normally hands the frontend a pointer and a size per memory
domain, once, straight after `Init`. Symbian memory cannot be described that
way and the reason is worth writing down: **there is no block**. The memory
model gives every chunk its own host mapping, so "the machine's RAM" is a set of
mappings rather than a region - and a game's chunks do not exist at `Init` at
all. `Init` launches the application, but launching only makes the process and
its first thread; nothing has run, and Red Faction's heap (32 MiB reserved at
0x00700000, about 7 MiB of it committed once it has loaded) appears somewhere in
the first few hundred frames. A pointer captured at `Init` would point at
nothing.

So the core declares a **bus** instead - the optional tooling ABI's address
space, resolved per access - and the frontend turns it into a callback-backed
domain that behaves like any other in the hex editor and the watch tools.
`PeekBus`/`PokeBus` resolve each address through the page tables of the
application this machine started (the asid is taken at launch, not looked up per
access: by the time anybody asks, the current thread is as likely to be the
window server's as the game's). Sixty-four megabytes from zero, which is where
EKA1 puts a process's own memory and everything this game maps. Addresses are
the machine's own, so a watch address is what a debugger would call it.

One page of translation is cached. A search walks addresses in order, so all but
one byte in four thousand is answered without touching the page tables: reading
the whole 64 MiB space costs about a third of a second.

**What the bus found:** the emulator keeps a little of its own bookkeeping inside
guest chunks. Eight-byte host pointers, at the base of the chunks it allocates
through - 134 bytes of the 67 million, at six chunk bases. They are the only
thing in the machine's memory that differs between a native process and the
sandbox, and they differ by construction rather than by accident. The gate
therefore compares the BODY of the game's heap (0x00701000 to 0x02A00000), which
holds nine tenths of everything the machine has written and matches exactly:
`bus body: digest d3f20d577255bb28 nonzero 788881`, both flavors.

The exports ship in `core.wbx` (`GetBusCount`, `GetBusName`, `GetBusSize`,
`GetBusWritable`, `PeekBus`, `PokeBus`) and the gate drives them through the
same address-resolution the engine uses. What is NOT yet proven is the frontend
half: `chimera-run --dump` only knows the pointer-backed domains, so a bus
cannot be dumped headlessly without a change to chimera itself.

## The project is the game, and the phone is firmware (2026-09-06)

Asked for by the user, and right: one ROM serves every game, so a project should
name the game it is about. The `rom` slot is gone. The project's file is the
GAME - a card dump, a Symbian package, or a `.blz` - and the machine it runs on
arrives through the firmware channel:

- **`sym.rom`**, always required. Any Symbian phone's dump; nothing is pinned,
  because every model's is a different file.
- **`blzinstapp.sis`**, required only when the project's file is a `.blz`
  (`requiredWhen: {slot: game, extension: blz}`). Pinned by size and SHA1:
  there is exactly one such installer.

## A .blz installs itself (2026-09-06)

A `.blz` is an `NGPK` container: compressed, undocumented, and nothing in this
core or in EKA2L1 can read one. What CAN read one is a Symbian application -
BLZinstapp - so the machine runs it. `Init` puts the `.blz` on drive E where the
application looks (`e:\*.blz`), installs the application onto drive C, starts
it, and works its two-key menu: left soft key for Options, left soft key again
for Install. It unpacks the game into the machine's own memory, exits, and the
game that appeared is the one the machine starts. All of that happens before the
first frame anybody asked for, so a movie begins with the game running.

Red Faction from a card and MotoGP from a `.blz` both run through `chimera-run`
against the packaged core, with the ROM and the installer supplied as firmware.

**Three real bugs stood in the way, all ours, all found by this one application:**

1. **A seek to "address" has to be REFUSED, not answered.** Symbian asks a file
   where it lives in memory so it can be read in place; the file server treats
   sixty-four bits of all-ones as "nowhere" and anything else as an address it
   hands to the guest. The machine's own filesystem answered `0xFFFFFFFF` -
   thirty-two bits - so the application read its own resource file from address
   0xFFFFFFFF and died with KERN-EXEC 3. Every user-interface application on a
   machine-memory drive would have.
2. **Opening for writing must not empty the file when reading was asked for
   too.** The host filesystem opens `"rb+"` for `RFile::Open(EFileWrite)` and
   `"wb+"` only for `RFile::Replace`; ours truncated for both. The installer
   streams its source - reads a chunk, writes it out, shortens the source -
   through exactly such a handle, and got an empty file. It called that a
   corrupt file, which it was, and we had made it one.
3. **An installer's work is invisible until the drive is announced.** A package
   installed onto drive C never reached the application list, so nothing could
   start what a `.sis` project installed either.

## A driver that draws nowhere (2026-09-06)

The sandbox has no OpenGL, and until now it had no graphics driver at all. That
was survivable only because a game drawing through direct screen access paints
the panel itself. Anything that asks the WINDOW SERVER to compose - every
user-interface application, the BLZ installer among them - goes through the
driver pointer to make a bitmap or a glyph atlas, and a machine with no driver
does not draw nothing there: it dies.

So the machine always has one now (`waterbox/null-graphics.h`): a driver that
accepts every command, executes none, and answers the ones that ask for a handle
with a number of its own. It also FREES what each command owns - a command
carrying pixels hands them over, and the real driver deletes them after the
upload. Ignoring that leaked every frame MotoGP drew, and 256 MiB of guest heap
lasted about forty seconds.

The native reference uses the same driver whenever it is not asked for a picture,
so both flavors are the same machine: `1,050,990,966` instructions and screen
`18db086fd87c329e` on either side.

## The machine's own OpenGL (2026-09-06)

Not every game paints the panel itself. King Of Fighters draws through the
WINDOW SERVER - its visible window is a redraw canvas, a recorded list of draw
commands that only exist as pixels once something composes them - and composing
needs an OpenGL. The sandbox has no GPU to borrow one from, so the core carries
its own: **Mesa 24.0.9, OSMesa front end, softpipe, no LLVM**, compiled into
core.wbx (`waterbox/gl-osmesa.c`). softpipe is plain C with no JIT and no
dispatch on host CPU features, so what it draws is decided entirely by code we
compiled, which is the only kind of renderer a core that replays movies can use.

The recipe and its five patches are not ours - they are
`~/minihawk-tools/mesa-guest/`, proven on 2026-08-28 and already carried by
three other cores. What this core had to add was one bug of its own:

**A driven driver must never wait.** `pump()` took the driver's command queue
with `pop(1)` - a one microsecond timed wait when the queue is empty. On a host
that returns; in the sandbox the guest parks on the futex and nothing ever wakes
it, because there is no other thread to put work in. miniBox says it plainly:
"all threads fell asleep. states: t1=W". Patch 0027 gives the queue a `try_pop`
that never waits and has `pump()` use it.

**The result:** King Of Fighters composes inside the box, and the picture is
byte for byte what the host's own OpenGL draws (`screen: 176x208 digest
8428baf32f991081 lit 33363`) - llvmpipe and softpipe agree on this content,
which is blits rather than shading. The machine is untouched by any of it:
242,334,156 instructions with the null driver, with the host's GL, and with
Mesa in the box.

**What it costs:** the package goes from 8 MB to 12.3 MB. The linked core is 44
MB, two thirds of which was Mesa's DWARF until `strip --strip-debug` (the
symbol table stays: the host resolves this core's exports out of it).

## The other games (2026-09-06)

The user's four new cards, on this ROM:

- **FIFA 2005** works, and always did.
- **King Of Fighters** works now, and is why the OpenGL above exists.
- **Tomb Raider** unpacks from its .blz and starts, then asks for
  `whichservice.dll` - which is nowhere in this ROM image; the loader says so
  and the game puts up "System error" and quits. It needs a fuller N-Gage dump,
  not a core change.
- **FIFA 2004** branches through a garbage function pointer (`blx r12` with
  r12 = 0x1b) about 1.4 million instructions in, right after opening its save
  files through the POSIX service. It fails identically on EKA2L1's own
  filesystem with none of this core involved, so it is an upstream
  compatibility gap rather than anything here. Not TLS: the EKA1 executor's
  set_tls (0x34) is called and every later lookup finds its slot.

## An EKA2 phone and N-Gage 2.0 (chimera#133, 2026-09-23)

An N-Gage 2.0 game is not an N-Gage game in the old sense. It runs on a Symbian
9 phone - a Nokia 5320 XpressMusic is the tested one - inside the N-Gage
application, which installs it from an `.n-gage` file (a MIME multipart: a
retailer note and two SIS packages) placed in `E:\n-gage\`. So a project brings
three things as firmware and one as the game:

- `SYM.ROM` and `SYM.RPKG`. An EKA2 phone keeps only its kernel and core
  programs in the ROM image; the rest of drive Z is a second image (ROFS),
  dumped as an RPKG. The RPKG is uncompressed - an index of files and their
  bytes - so it is read once and SERVED as drive Z from memory, each file
  pointing into the buffer, read-only, and registered BEHIND the ROM's own
  filesystem (set_device), which is the order the emulator's desktop install
  gives the ROM and the unpacked folder. Nothing is unpacked into the machine.
  The buffer IS in every savestate, though (a FIFA 09 state is 135 MB against
  a 121 MB RPKG): it is ordinary heap, and nothing yet tells the sandbox that
  it never changes. Open. Which phone it is comes from the RPKG's version files, with the
  same fallbacks as the desktop install (patch 0031); a v1 RPKG carries no
  machine UID and neither install passes one.
- `ngage.sis`, the N-Gage 2.0 application (v1.40.1557, pinned by SHA1). The
  "patch" SISX that circulates with it is NOT needed: FIFA 09 reaches the same
  picture with and without it.
- The `.n-gage`. `install_ngage` puts it in `E:\n-gage\`, installs the
  application and starts its Games view (playserver, 0x20007B39); the
  application installs the game itself, as a phone does, and shows it in My
  Games with Start Game on the left soft key. That press is the movie's.

**What drive Z from memory broke, one at a time, each found by the machine:**

1. The window server's redraw from outside the scheduler returned holding the
   scheduler's lock, and the next redraw waited forever (0029, and
   `redraw_now` in the core). Latent since the compositor went in; the N-Gage
   application was the first thing to draw through the window server here.
2. A window group kept a raw pointer to the process that started its owner;
   the installer exits first, and the group then unregistered from freed
   memory (0030, by kernel id).
3. The central repository read `.txt` repositories by host path only (0032),
   the DRM rights database was an SQLite file by host path only (0033, kept in
   memory - and `flush()`, which closes and reopens, must not throw an
   in-memory database away), and the ROM's SIS stubs were registered by host
   path only (0034). Without the stubs the game's dependency on the S60 3.2
   platform could not be met, and the application said "Game not found".
4. **memfs deleted a directory with everything in it.** The phone's own
   integrity service tidies up by removing each directory on the way back to
   the root and counts on RmDir refusing a directory that is not empty. It took
   `C:\sys` - and every installed program - with it.
5. The languages a device reads from `languages.txt` came from a host path;
   the machine now reads the same file from drive Z (English was the fallback).

**Determinism, and what it took.** The phone ran a different machine every run.
Four sources, each proven by bisecting instruction counts between two runs:

- `eka2l1::random_range` seeded a new generator from `std::random_device` on
  every call (temp file names, a Bluetooth address) - one fixed-seed generator
  now (0035).
- The locale was built on the stack uninitialised: its language downgrade path
  was host stack garbage, so the machine looked for its resource files in a
  different language every run (0036).
- Host structs handed to the guest by the emulated services (a volume's info,
  a directory entry, a phone's network, an app's info - a dozen sites found by
  scanning descriptor writes for the host's stack canary) carried their unset
  fields and padding. `-ftrivial-auto-var-init=zero` on the whole build closes
  every such site, including those not found.
- The MIDI engine's clock was the host's wall clock (0037).

With those, two sandbox runs of FIFA 09 are byte-identical (instructions and
the address space), and so are two native runs. The core also pins the
machine's time zone to UTC: the emulator reads the phone's UTC offset and zone
name from the host's C library, so a movie would have run a different machine
in another country. The sandbox had no zone and answered UTC already.

**Open: native and sandbox are not the same machine on an EKA2 phone.** They
draw the same picture, and each is deterministic, but they part by 17
instructions at the fifth frame and never meet again. Ruled out: the file names
the harness hands over, the start order of graphics and audio, the GL driver,
the compiler (a GCC 13 native build parts the same way), the C library's
case folding. Also found and not yet removed from the native reference: the
length of its storage path reaches the machine (the gate runs it with the
sandbox's `data`). The gate holds determinism, a savestate across processes and
the picture, and not the instruction count, until this is traced.
