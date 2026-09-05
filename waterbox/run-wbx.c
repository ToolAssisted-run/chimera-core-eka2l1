/* Standalone driver for the waterboxed EKA2L1 core: runs core.wbx through the
 * miniBox host and reports what the machine did in run-native's exact format,
 * so the sandboxed build can be diffed against the native reference.
 *
 * usage: run-wbx <core.wbx> [--frames N] [--rom SYM.ROM] [--run 0xUID]
 *
 * Without a ROM the machine inside the box is the empty one and its workload is
 * a kernel timer every millisecond, which is what run-native drives with
 * `--frames N --timer-us 1000`. With one, that single file is the whole device:
 * it says which device it is and it carries drive Z.
 */
#include "minibox.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	FILE *f;
} freader;

static intptr_t file_read(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	return (intptr_t)fread(d, 1, s, ((freader *)ud)->f);
}

typedef int (MB_GUEST_ABI *intfn)(void);
typedef void (MB_GUEST_ABI *framefn)(uint64_t);
typedef uint64_t (MB_GUEST_ABI *u64fn)(void);
typedef uintptr_t (MB_GUEST_ABI *ptrfn)(void);

static uintptr_t proc(mb_host *h, const char *n)
{
	mb_return r;
	wbx_get_proc_addr(h, n, &r);
	if (r.error_message[0]) { fprintf(stderr, "proc %s: %s\n", n, r.error_message); exit(2); }
	if (!r.data) { fprintf(stderr, "missing required export %s\n", n); exit(2); }
	return r.data;
}

int main(int argc, char **argv)
{
	const char *core = NULL, *rom = NULL, *game = NULL;
	long frames = 60, timer_us = 0;
	uint32_t run_uid = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
		else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
		else if (!strcmp(argv[i], "--timer-us") && i + 1 < argc) timer_us = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--run") && i + 1 < argc) run_uid = (uint32_t)strtoul(argv[++i], NULL, 16);
		else if (argv[i][0] != '-' && !core) core = argv[i];
		else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
	}

	if (!core) { fprintf(stderr, "usage: run-wbx <core.wbx> [--frames N]\n"); return 2; }

	FILE *wf = fopen(core, "rb");
	if (!wf) { fprintf(stderr, "cannot read %s\n", core); return 1; }

	/* The empty machine is small: the emulator's own heap, and nothing
	 * emulated yet. The mmap arena is the reservation a device's memory
	 * model will come out of. */
	mb_memory_layout_template layout = { 256u << 20, 16u << 20, 64u << 20, 64u << 20, (uintptr_t)4 << 30 };
	freader fr = { wf };
	mb_return r;

	wbx_create_host(&layout, "core.wbx", file_read, (uintptr_t)&fr, &r);
	fclose(wf);
	if (r.error_message[0]) { fprintf(stderr, "create: %s\n", r.error_message); return 1; }
	mb_host *h = (mb_host *)r.data;

	printf("host created\n"); fflush(stdout);
	/* The device, if there is one: one file, under the name the guest opens
	 * its firmware by. */
	if (rom) {
		wbx_mount_file_path(h, "rom", rom, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount rom: %s\n", r.error_message); return 1; }
	}

	if (game) {
		wbx_mount_file_path(h, "game", game, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount game: %s\n", r.error_message); return 1; }
	}

	intfn Init = (intfn)proc(h, "Init");
	ptrfn GetLoadError = (ptrfn)proc(h, "GetLoadError");

	if (Init() != 1) {
		fprintf(stderr, "Init failed: %s\n", (const char *)GetLoadError());
		return 1;
	}

	printf("init ok\n"); fflush(stdout);
	framefn FrameAdvance = (framefn)proc(h, "FrameAdvance");
	u64fn GetVirtualUs = (u64fn)proc(h, "GetVirtualUs");
	u64fn GetInstructions = (u64fn)proc(h, "GetInstructions");
	u64fn GetTimerFired = (u64fn)proc(h, "GetTimerFired");
	u64fn GetTimerLastUs = (u64fn)proc(h, "GetTimerLastUs");
	u64fn GetTimerLatenessUs = (u64fn)proc(h, "GetTimerLatenessUs");

	/* seal: the post-boot machine is the savestate baseline */
	wbx_deactivate_host(h, &r);
	wbx_seal(h, &r);
	if (r.error_message[0]) { fprintf(stderr, "seal: %s\n", r.error_message); return 1; }
	wbx_activate_host(h, &r);

	if (timer_us > 0) {
		typedef void (MB_GUEST_ABI *timerfn)(int32_t);
		timerfn SetTimerCheckUs = (timerfn)proc(h, "SetTimerCheckUs");
		SetTimerCheckUs((int32_t)timer_us);
	}

	if (run_uid) {
		typedef int (MB_GUEST_ABI *runfn)(uint32_t);
		runfn LaunchAppUid = (runfn)proc(h, "LaunchAppUid");
		printf("run: 0x%08x %s\n", run_uid, LaunchAppUid(run_uid) ? "started" : "refused");
	}

	for (long f = 0; f < frames; f++) FrameAdvance(0);

	u64fn GetDriveEntries = (u64fn)proc(h, "GetDriveEntries");
	u64fn GetDriveWrittenBytes = (u64fn)proc(h, "GetDriveWrittenBytes");
	intfn GetDeviceMounted = (intfn)proc(h, "GetDeviceMounted");

	u64fn GetAppCount = (u64fn)proc(h, "GetAppCount");

	intfn GetInstallResult = (intfn)proc(h, "GetInstallResult");

	printf("device: %s\n", GetDeviceMounted() ? "mounted" : "none");
	printf("install: %d\n", GetInstallResult());
	printf("apps: %" PRIu64 "\n", GetAppCount());
	printf("drive entries: %" PRIu64 "\n", GetDriveEntries());
	printf("drive written: %" PRIu64 " bytes\n", GetDriveWrittenBytes());
	printf("frames: %ld at 60 fps\n", frames);
	printf("virtual us: %" PRIu64 "\n", GetVirtualUs());
	printf("instructions: %" PRIu64 "\n", GetInstructions());
	printf("timer fired: %" PRIu64 " every 1000 us\n", GetTimerFired());
	printf("timer last: %" PRIu64 " us\n", GetTimerLastUs());
	printf("timer lateness: %" PRIu64 " us\n", GetTimerLatenessUs());

	wbx_destroy_host(h, &r);
	return 0;
}
