// The gaps between what the emulator's dependencies expect of a C library and
// what the sandbox's musl provides. Everything here is a real definition, not
// a weak alias: a static link means these shadow whatever musl would have
// supplied, and the guest is the only thing running.
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern "C" {

// libarchive calls this before exec'ing a decompression helper, which cannot
// happen here: there is no fork, no exec and no file descriptor to inherit.
// musl does not ship it, so the reference is unresolved without this.
void closefrom(int lowfd) {
    (void)lowfd;
}

// The sandbox serves clock_gettime but not clock_getres, and libstdc++'s
// chrono asks for the resolution of every clock it knows before anything else
// runs. The answer is a nanosecond for all of them - which is true of the
// machine's clock, since it is exact - and no clock here is the host's anyway.
int clock_getres(clockid_t clock, struct timespec *res) {
    (void)clock;

    if (res) {
        res->tv_sec = 0;
        res->tv_nsec = 1;
    }

    return 0;
}
}

extern "C" {

// The sandbox has one flat namespace of mounted files and no notion of a
// working directory. Answering "/" keeps the path code that asks - anything
// resolving a relative path to an absolute one - working on names that are
// already the whole path.
char *getcwd(char *buf, size_t size) {
    if (!buf) {
        errno = EINVAL;
        return nullptr;
    }

    if (size < 2) {
        errno = ERANGE;
        return nullptr;
    }

    buf[0] = '/';
    buf[1] = '\0';

    return buf;
}
}

// The sandbox holds a flat set of mounted files, with no directories and no
// permissions. The emulator lays out a storage root as it starts - drives,
// caches, registries - and each of those calls has to answer something. They
// answer success: the tree it believes it made is the tree it will find,
// because every name in it resolves to a mounted file or to nothing at all.
extern "C" {

int mkdir(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    return 0;
}

int mkdirat(int fd, const char *path, mode_t mode) {
    (void)fd;
    (void)path;
    (void)mode;
    return 0;
}

int rmdir(const char *path) {
    (void)path;
    return 0;
}

int chdir(const char *path) {
    (void)path;
    return 0;
}

int chmod(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    return 0;
}

mode_t umask(mode_t mask) {
    (void)mask;
    return 0;
}
}

extern "C" {

// The sandbox serves stat but not lstat, and there are no symbolic links in a
// flat list of mounted files: asking about the link is asking about the file.
int lstat(const char *path, struct stat *out) {
    return stat(path, out);
}
}

#include <fcntl.h>
#include <stdarg.h>
#include <sys/syscall.h>

extern "C" {

// Nothing here execs, and every descriptor belongs to the sandbox, so
// close-on-exec is a property with nobody to observe it.
//
// musl's open() asks the kernel for O_CLOEXEC by making a SECOND call - a raw
// fcntl, not the fcntl below, which is why defining that alone did not help.
// This strips the flag before the open ever happens.
int open(const char *path, int flags, ...) {
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }

    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags & ~O_CLOEXEC, mode));
}

// And the same question asked directly. There is no exec here and every
// descriptor is the sandbox's own, so the flags are whatever the caller last
// said and nothing acts on them.
int fcntl(int fd, int cmd, ...) {
    (void)fd;

    switch (cmd) {
    case F_GETFD:
    case F_GETFL:
        return 0;

    case F_SETFD:
    case F_SETFL:
        return 0;

    default:
        errno = EINVAL;
        return -1;
    }
}
}
