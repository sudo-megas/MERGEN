// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only
//
// Privileged reader for PDFs the invoking user cannot open. Run through
// pkexec, never directly, and never installed setuid.
//
// The polkit action authorises reading *a PDF*, so this program enforces
// exactly that and nothing wider: it opens the path, and only once it holds
// the descriptor does it decide whether the thing on the other end qualifies.
// Validating the descriptor rather than the path leaves no window in which the
// name could be swapped for another file. A root-owned file that is not a PDF
// is refused, so the action cannot be turned into a general-purpose read of
// arbitrary privileged files.
//
// Output is the file's bytes on stdout. Diagnostics go to stderr.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Large enough for any document a person opens by hand, small enough that a
// device file mistaken for a document cannot exhaust memory downstream.
constexpr long long kMaxBytes = 2LL * 1024 * 1024 * 1024;

constexpr char kMagic[] = "%PDF-";
constexpr size_t kMagicLen = sizeof(kMagic) - 1;

int fail(const char *what) {
    std::fprintf(stderr, "mergen-open: %s\n", what);
    return 1;
}

bool writeAll(const char *buf, size_t len) {
    while (len > 0) {
        const ssize_t n = ::write(STDOUT_FILENO, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        buf += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return fail("expects exactly one argument: an absolute path to a PDF");
    }
    const char *path = argv[1];
    if (path[0] != '/') {
        return fail("path must be absolute");
    }

    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return fail(std::strerror(errno));
    }

    // Everything below inspects the descriptor, not the name.
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return fail(std::strerror(errno));
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return fail("not a regular file");
    }
    if (st.st_size > kMaxBytes) {
        ::close(fd);
        return fail("file is too large");
    }

    char magic[kMagicLen];
    ssize_t got = 0;
    while (got < static_cast<ssize_t>(kMagicLen)) {
        const ssize_t n = ::read(fd, magic + got, kMagicLen - static_cast<size_t>(got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return fail(std::strerror(errno));
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    if (got < static_cast<ssize_t>(kMagicLen) || std::memcmp(magic, kMagic, kMagicLen) != 0) {
        ::close(fd);
        return fail("not a PDF");
    }

    if (!writeAll(magic, kMagicLen)) {
        ::close(fd);
        return fail("write failed");
    }

    // st_size was a snapshot, and a file that grows while it is read would
    // otherwise be copied without bound. The helper's job is one file's bytes,
    // so it emits what the file declared and no more — whichever of the two
    // limits is tighter.
    const long long limit = st.st_size < kMaxBytes ? st.st_size : kMaxBytes;
    long long sent = static_cast<long long>(kMagicLen);
    char buf[64 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            return fail(std::strerror(errno));
        }
        if (n == 0) {
            break;
        }
        sent += n;
        if (sent > limit) {
            // Truncate rather than emit bytes the caller was never told about.
            ::close(fd);
            return fail("file changed while it was being read");
        }
        if (!writeAll(buf, static_cast<size_t>(n))) {
            ::close(fd);
            return fail("write failed");
        }
    }

    ::close(fd);
    // A short read is not a successful read. st_size was a snapshot; if the
    // file shrank underneath us the loop ends early with no error to report,
    // and the caller would open a truncated document believing it whole.
    // poppler reconstructs a broken xref rather than refusing, so nothing
    // downstream would notice. Say so here, where the truth is still known.
    if (sent != limit) {
        ::close(fd);
        return fail("file changed while it was being read");
    }

    return 0;
}
