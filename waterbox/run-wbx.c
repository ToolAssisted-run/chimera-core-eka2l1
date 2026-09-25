/* Standalone driver for the waterboxed EKA2L1 core: runs core.wbx through the
 * miniBox host and reports what the machine did in run-native's exact format,
 * so the sandboxed build can be diffed against the native reference.
 *
 * usage: run-wbx <core.wbx> [--frames N] [--rom SYM.ROM] [--rpkg SYM.RPKG] [--run 0xUID]
 *                [--rerecord] [--state-out FILE] [--state-in FILE] [--press N]
 *
 * --rerecord saves and reloads the machine before every single frame. If any
 * of the machine lives outside the sandbox's memory - or the core keeps a
 * pointer across a load - the run diverges from an ordinary one.
 *
 * --state-out writes the machine to a file once the frames are done, and
 * --state-in starts from one instead of launching anything: a state written by
 * one process and read by another, which is what a movie asks of a core.
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

/* a mounted file that is only bytes in memory - the slots map */
typedef struct {
	const uint8_t *p;
	size_t len, pos;
} freader_mem;

static intptr_t mem_read(uintptr_t ud, uint8_t *d, uintptr_t s)
{
	freader_mem *m = (freader_mem *)ud;
	size_t left = m->len - m->pos;

	if (s > left) s = left;

	memcpy(d, m->p + m->pos, s);
	m->pos += s;
	return (intptr_t)s;
}

/* The savestate, in memory: written into on save, read back on load. */
/* The bus this driver reads, and the window of it worth comparing. */
#define BUS_SIZE       0x04000000
#define BUS_BODY_FIRST 0x00701000
#define BUS_BODY_LAST  0x02A00000

struct statebuf { uint8_t *p; size_t len, cap, pos; };
static struct statebuf g_state;

static int32_t state_write(uintptr_t ud, const uint8_t *d, uintptr_t n)
{
	struct statebuf *b = (struct statebuf *)ud;

	if (b->len + n > b->cap) {
		size_t want = (b->len + n) * 2;
		uint8_t *q = (uint8_t *)realloc(b->p, want);

		if (!q) return -1;

		b->p = q; b->cap = want;
	}

	memcpy(b->p + b->len, d, n);
	b->len += n;
	return 0;
}

static intptr_t state_read(uintptr_t ud, uint8_t *d, uintptr_t n)
{
	struct statebuf *b = (struct statebuf *)ud;
	size_t left = b->len - b->pos;

	if (n > left) n = left;

	memcpy(d, b->p + b->pos, n);
	b->pos += n;
	return (intptr_t)n;
}

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
	const char *core = NULL, *rom = NULL, *game = NULL, *installer = NULL, *rpkg = NULL, *ngage_launcher = NULL;
	long frames = 60, timer_us = 0;
	uint32_t run_uid = 0;
	int rerecord = 0;
	const char *state_out = NULL, *state_in = NULL;
	long press = -1;
	const char *bus_out = NULL;
	const char *settings_json = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
		else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
		else if (!strcmp(argv[i], "--blz-installer") && i + 1 < argc) installer = argv[++i];
		else if (!strcmp(argv[i], "--rpkg") && i + 1 < argc) rpkg = argv[++i];
		else if (!strcmp(argv[i], "--ngage-launcher") && i + 1 < argc) ngage_launcher = argv[++i];
		else if (!strcmp(argv[i], "--timer-us") && i + 1 < argc) timer_us = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--run") && i + 1 < argc) run_uid = (uint32_t)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--rerecord")) rerecord = 1;
		else if (!strcmp(argv[i], "--state-out") && i + 1 < argc) state_out = argv[++i];
		else if (!strcmp(argv[i], "--state-in") && i + 1 < argc) state_in = argv[++i];
		else if (!strcmp(argv[i], "--press") && i + 1 < argc) press = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--bus-out") && i + 1 < argc) bus_out = argv[++i];
		else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settings_json = argv[++i];
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
		/* firmware, under the id the core declares for it */
		wbx_mount_file_path(h, "sym.rom", rom, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount rom: %s\n", r.error_message); return 1; }
	}

	if (rpkg) {
		/* an EKA2 phone's drive Z, beside its ROM */
		wbx_mount_file_path(h, "sym.rpkg", rpkg, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount rpkg: %s\n", r.error_message); return 1; }
	}
	if (ngage_launcher) {
		/* the N-Gage application, which installs a .n-gage */
		wbx_mount_file_path(h, "ngage.sis", ngage_launcher, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount ngage launcher: %s\n", r.error_message); return 1; }
	}
	if (installer) {
		/* the other firmware: the application that unpacks a .blz */
		wbx_mount_file_path(h, "blzinstapp.sis", installer, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount installer: %s\n", r.error_message); return 1; }
	}

	/* The game, exactly as a chimera project hands it over: mounted under its
	 * own file name, with a "slots" map that says which slot that name fills.
	 * The name matters - a .blz is told apart from a card dump by it. */
	char slots_json[512] = { 0 };

	if (game) {
		const char *leaf = strrchr(game, '/');

		leaf = leaf ? leaf + 1 : game;

		wbx_mount_file_path(h, leaf, game, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount game: %s\n", r.error_message); return 1; }

		snprintf(slots_json, sizeof slots_json, "{\"game\":[\"%s\"]}", leaf);

		freader_mem slots_reader = { (const uint8_t *)slots_json, strlen(slots_json), 0 };
		wbx_mount_file(h, "slots", mem_read, (uintptr_t)&slots_reader, 0, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount slots: %s\n", r.error_message); return 1; }
	}

	/* the project's settings, as a project hands them over: a mounted JSON */
	freader_mem settings_reader = { 0 };
	if (settings_json) {
		settings_reader.p = (const uint8_t *)settings_json;
		settings_reader.len = strlen(settings_json);
		wbx_mount_file(h, "settings", mem_read, (uintptr_t)&settings_reader, 0, &r);
		if (r.error_message[0]) { fprintf(stderr, "mount settings: %s\n", r.error_message); return 1; }
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

	/* A state carries the running application with it, so a machine started
	 * from one launches nothing. */
	if (state_in) {
		FILE *sf = fopen(state_in, "rb");
		mb_return sr;

		if (!sf) { fprintf(stderr, "cannot read %s\n", state_in); return 1; }

		fseek(sf, 0, SEEK_END);
		g_state.len = (size_t)ftell(sf);
		fseek(sf, 0, SEEK_SET);
		g_state.p = (uint8_t *)malloc(g_state.len);
		g_state.cap = g_state.len;
		g_state.pos = 0;

		if (!g_state.p || fread(g_state.p, 1, g_state.len, sf) != g_state.len) {
			fprintf(stderr, "short read of %s\n", state_in); return 1;
		}

		fclose(sf);
		wbx_load_state(h, state_read, (uintptr_t)&g_state, &sr);
		if (sr.error_message[0]) { fprintf(stderr, "load_state: %s\n", sr.error_message); return 1; }

		printf("state in: %zu bytes\n", g_state.len);
	} else if (run_uid) {
		typedef int (MB_GUEST_ABI *runfn)(uint32_t);
		runfn LaunchAppUid = (runfn)proc(h, "LaunchAppUid");
		printf("run: 0x%08x %s\n", run_uid, LaunchAppUid(run_uid) ? "started" : "refused");
	}

	typedef void (MB_GUEST_ABI *btnfn)(int32_t, int32_t);
	btnfn SetButton = (btnfn)proc(h, "SetButton");

	for (long f = 0; f < frames; f++) {
		/* Held near the end, exactly as the native reference holds it: what a
		 * key did is read off the last frame's screen. */
		if (press >= 0) {
			if (f == frames - 200) SetButton((int32_t)press, 1);
			else if (f == frames - 180) SetButton((int32_t)press, 0);
		}

		if (rerecord) {
			mb_return sr;

			g_state.len = 0;
			wbx_save_state(h, state_write, (uintptr_t)&g_state, &sr);
			if (sr.error_message[0]) { fprintf(stderr, "save_state: %s\n", sr.error_message); return 1; }

			g_state.pos = 0;
			wbx_load_state(h, state_read, (uintptr_t)&g_state, &sr);
			if (sr.error_message[0]) { fprintf(stderr, "load_state: %s\n", sr.error_message); return 1; }
		}

		FrameAdvance(0);
	}

	{
		/* The address space, digested exactly as the native reference digests
		 * it: the whole space counted, and the body of the game's heap - the
		 * part with none of the emulator's own bookkeeping in it - digested. */
		typedef int32_t (MB_GUEST_ABI *peekfn)(int32_t, int32_t);
		peekfn PeekBus = (peekfn)proc(h, "PeekBus");
		uint64_t digest = 1469598103934665603ull;
		size_t mapped = 0, body = 0;

		for (int32_t addr = 0; addr < BUS_SIZE; addr++) {
			uint8_t byte = (uint8_t)PeekBus(0, addr);

			if (byte) mapped++;

			if (addr >= BUS_BODY_FIRST && addr < BUS_BODY_LAST) {
				digest ^= byte;
				digest *= 1099511628211ull;

				if (byte) body++;
			}
		}

		printf("bus: 64 MiB nonzero %zu\n", mapped);
		printf("bus body: digest %016llx nonzero %zu\n", (unsigned long long)digest, body);

		if (bus_out) {
			FILE *bf = fopen(bus_out, "wb");

			if (bf) {
				for (int32_t addr = 0; addr < BUS_SIZE; addr++) {
					uint8_t byte = (uint8_t)PeekBus(0, addr);
					fwrite(&byte, 1, 1, bf);
				}

				fclose(bf);
			}
		}
	}

	if (rerecord) printf("state bytes: %zu\n", g_state.len);

	if (state_out) {
		FILE *sf = fopen(state_out, "wb");
		mb_return sr;

		if (!sf) { fprintf(stderr, "cannot write %s\n", state_out); return 1; }

		g_state.len = 0;
		wbx_save_state(h, state_write, (uintptr_t)&g_state, &sr);
		if (sr.error_message[0]) { fprintf(stderr, "save_state: %s\n", sr.error_message); return 1; }

		fwrite(g_state.p, 1, g_state.len, sf);
		fclose(sf);
		printf("state out: %zu bytes\n", g_state.len);
	}

	u64fn GetDriveEntries = (u64fn)proc(h, "GetDriveEntries");
	u64fn GetDriveWrittenBytes = (u64fn)proc(h, "GetDriveWrittenBytes");
	intfn GetDeviceMounted = (intfn)proc(h, "GetDeviceMounted");

	u64fn GetAppCount = (u64fn)proc(h, "GetAppCount");

	{
		/* The project's own application starts itself; say which one, in
		 * run-native's format, so the two can be compared. */
		typedef uint32_t (MB_GUEST_ABI *u32fn)(void);
		u32fn GetLaunchedAppUid = (u32fn)proc(h, "GetLaunchedAppUid");
		const uint32_t launched = GetLaunchedAppUid();

		if (launched) printf("launched: 0x%08x\n", launched);
	}

	/* The picture, in run-native's exact format. The machine draws it into
	 * its own memory, so the sandbox has one without any OpenGL at all and
	 * it must be the native reference's pixel for pixel. */
	{
		typedef uint32_t *(MB_GUEST_ABI *pixfn)(void);
		pixfn GetVideoBgra = (pixfn)proc(h, "GetVideoBgra");
		intfn GetVideoWidth = (intfn)proc(h, "GetVideoWidth");
		intfn GetVideoHeight = (intfn)proc(h, "GetVideoHeight");

		const int w = GetVideoWidth(), hgt = GetVideoHeight();
		const uint32_t *pixels = GetVideoBgra();
		uint64_t digest = 1469598103934665603ull;
		size_t lit = 0;

		for (long i = 0; i < (long)w * hgt; i++) {
			digest ^= pixels[i];
			digest *= 1099511628211ull;

			if (pixels[i] & 0x00FFFFFF) lit++;
		}

		printf("screen: %dx%d digest %016llx lit %zu\n", w, hgt,
			(unsigned long long)digest, lit);
	}

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
