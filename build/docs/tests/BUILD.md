# Acceptance suites — how to build and run

These are the milestone acceptance tests for Z1–Z10 and the small probes used
along the way. They were written in the job's scratch directory, which is
ephemeral; this is the durable copy.

They are NOT part of the CMake build. Each links against the objects the real
build already produced, swapping out `main.cpp.o` for the test's own `main()`.

## Build one

    B=<repo>/build
    S=<repo>/src
    OBJS=$(find $B/CMakeFiles/mergen.dir -name '*.o' ! -name 'main.cpp.o')

    g++ -std=c++20 -O2 -DNDEBUG -fPIC -o z1check z1check.cpp $OBJS \
      -I$S -I$B -I$B/mergen_autogen/include \
      -DMERGEN_VERSION='"1.0.0"' -DMERGEN_HELPER_PATH='"/x"' \
      -DMERGEN_RELEASE_DATE='"2026-08-24"' \
      $(pkg-config --cflags --libs Qt6Widgets Qt6PrintSupport Qt6Network poppler-qt6 libqpdf)

## Run

    QT_QPA_PLATFORM=offscreen ./z1check

Exit code is the number of failed checks; the last line is the verdict.
`z8check.sh` is a shell script driving real processes over the control socket —
run it with `bash z8check.sh` from a directory holding the test PDFs.

## Test documents

The suites expect these in the working directory. They live in the MERGEN repo
root (untracked):

| file | what it is |
|---|---|
| `test.pdf` | plain 3-page document |
| `outline.pdf` | nested table of contents + an internal link on page 1 to page 3 |
| `outline-v2.pdf` | copy of outline.pdf with page 2 altered — for compare mode |
| `annot.pdf` | highlight, underline and square annotations |
| `colour.pdf` | flat colour swatches — for night mode |
| `evil.pdf` | carries embedded JavaScript and a form field |

They are written by small python scripts writing raw PDF; see the git history
of this session or regenerate from the structure in any of them.

## Under sanitizers

Configure a second tree and link the suites against ITS objects:

    cmake -S <repo> -B /tmp/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -Wno-dev
    cmake --build /tmp/asan

then use `-fsanitize=address,undefined -g` on the g++ line and point `-I`/`OBJS`
at `/tmp/asan`. Run with `ASAN_OPTIONS=detect_leaks=0` — Qt is noisy about
leaks at exit.

## Z11 additions

`z11check` needs one define the earlier suites do not:

    -DMERGEN_OPEN_PATH="\"$B/mergen-open\""

It runs the privileged helper directly (without pkexec, on a file the test user
can already read) to prove it delivers the file byte for byte. Without the
define the suite will not compile.

Careful with the quoting in a loop — `-DMERGEN_OPEN_PATH="$B/mergen-open"` puts
a bare path in the source and the compiler reports `'home' was not declared in
this scope`.

### Test documents added at Z11

| file | what it is | what needs it |
|---|---|---|
| `huge.pdf` | one page, 14400×14400 pt — the PDF maximum | the fit-mode floor check |
| `large.pdf` | 1000 pages of A4 | the cache-growth bound |
| `locked.pdf` | password-protected | the locked-document checks |

`huge.pdf` is generated, not found; the generator is inline in the Z11g work
and rebuilding it is four lines of Python writing a minimal xref.

### Working directory

**Run the suites from the repository root**, not from `docs/tests/`. They open
their documents by relative path. Run from the wrong directory and nine of
eleven fail with messages that look like real regressions (`annot.pdf opened`
FAIL, empty content hashes, `No such file or directory`).

### Under AddressSanitizer

One check is skipped: `z11check`'s RSS bound. ASan's quarantine inflates
resident memory far past the 40 MB threshold, so the check would fail for a
reason that has nothing to do with the code. It is guarded by
`__SANITIZE_ADDRESS__` and prints a line saying it was skipped.

### A measured Qt behaviour worth knowing

`notifier-probe.cpp` in this directory establishes something that matters for
any loop which runs the event queue by hand:

    g++ -std=c++20 -O1 -w -fPIC -o notifier-probe notifier-probe.cpp \
      $(pkg-config --cflags --libs Qt6Network Qt6Core)
    ./notifier-probe               # FAILS  - glib dispatcher
    QT_NO_GLIB=1 ./notifier-probe  # PASSES - UNIX dispatcher

`QEventLoop::ExcludeSocketNotifiers` **does not work** under
`QEventDispatcherGlib`, which is what Qt selects on any desktop Linux with GLib
available — that is, on MERGEN's actual target. It works exactly as documented
under `QEventDispatcherUNIX`.

This is why `printDocument` does not rely on the flag. It sets `m_printing`,
and `runCommand` refuses everything while that is set.
