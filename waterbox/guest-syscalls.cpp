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
