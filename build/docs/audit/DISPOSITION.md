# Disposition — every finding, and what was done about it

The freeze audit produced **51 findings, numbered to V52 with no V42**,
collapsing to 21 root causes. This table is the whole of it: each finding is **fixed**,
**rejected**, **ruled**, or **partial**, with the commit or the evidence.

Read this rather than the eleven journals. They are the working notes; this is
the answer.

Written after Z11j. Suites: 11/11 pass. CI audits: 3/3 pass. Clean build at
zero warnings with `-Wall -Wextra -Wpedantic`; ASan+UBSan clean.

---

## Summary

| disposition | findings |
|---|---|
| fixed, with a commit | 40 |
| ruled — redaction, withdrawn rather than shipped | 7 |
| rejected, or magnitude overstated | 2 |
| partial — bounded, not eliminated | 2 |
| **total** | **51** |

Two further rulings are recorded below that are not findings of their own:
where an elevated document may go (three routes, ruled differently), and two
standing structural notes — clazy hygiene and `readElevated`'s nested loop.

The seven ruled findings are all inside redaction, which was **withdrawn** at
Z11b rather than shipped. The code is still in the tree and still compiles; it
is unreachable from the interface.

---

## Fixed

| # | finding | commit |
|---|---|---|
| V1 | heap-use-after-free in redaction (`QByteArray` temporary into `QPDFWriter`) | Z11a |
| V2 | the shipped binary reported version 1.0.0 | Z11a — binary now carries `2.0.0` (verified with `strings -e l`) |
| V3 | PKGBUILD claimed integrity from a tag that is not signed | Z11b — the claim removed, not the tag faked |
| V4 | `audit-deps.sh` allowlist unanchored (`libm` matched `libmount`) | Z11b |
| V5 | audit scripts passed vacuously outside a git repository | Z11b — `git rev-parse` assertion |
| V6 | a symlink defeated redaction's "never writes over the source" guard | Z11b — device+inode, not string compare |
| V9 | SIGSEGV: a socket command during the password dialog | Z11c + Z11f |
| V10 | privileged plaintext reached disk via the coredump | Z11a — `prctl(PR_SET_DUMPABLE, 0)` |
| V11 | `readElevated` accepted a truncated privileged read as success | Z11g — helper compares sent against promised; reader checks `%PDF-` |
| V12 | `audit-colours.sh` caught 5 of 17 planted violations | Z11b — now 17/17 |
| V14 | one idle socket client froze the GUI thread for a second | Z11c — 5 idle clients: 5.0 s → 0.000 s |
| V15 | `pkexec` resolved through `$PATH` | Z11a — absolute `/usr/bin/pkexec` |
| V16 | per-page caches never pruned | Z11g — see *partial* note below, it is a bound not a repair |
| V17 | stale diff bands survived onto an unrelated document | Z11d |
| V18 | `computeDiff` blocked the UI 9.1 s on a 1000-page pair | Z11f — lazy provider, 9,100 ms → 12 ms |
| V21 | the helper's 2 GiB cap was never enforced during the copy | Z11a |
| V22 | heap-use-after-free in the control socket | Z11c |
| V23 | SEGV in the overlay via `deleteLater` inside a nested loop | Z11f — retired-widget holding, not `deleteLater` |
| V24 | process abort on window close during a search | Z11i — the *bounded* wait was the bug: three seconds could expire with the thread alive, and `~QObject` then destroys a running `QThread`, which is `qFatal`. Now waits properly. |
| V25 | a stale scroll animation dragged the new document | Z11d |
| V26 | privileged plaintext never wiped, outlived the document | Z11a — `wipeData()` |
| V28 | the socket raised an auth prompt then lied about the result | Z11e |
| V31 | the wipe asymmetry | Z11a (same fix as V26) |
| V32 | the crash's precise root cause (supersedes V9's diagnosis) | Z11c + Z11f |
| V33 | diff bands painted rotation-oblivious | Z11d — 510×28 unrotated vs 20×360 rotated |
| V34 | opening a document while comparing left compare dangling | Z11f |
| V36 | opening a document while presenting dropped the layout | Z11d |
| V37 | an uncaught SIGXFSZ killed the process | Z11a |
| V38 | `loadPortals`' `ends` counter silently destroyed portals | Z11e — record-based parse |
| V39 | a far-end rename broke `followPortal` though the hash was right there | Z11g |
| V40 | `markPortal` reported success when the save failed | Z11e |
| V41 | no cap on portals; `recent.toml`'s bracket scan was comment-blind | Z11e (cap) + Z11g (comments) |
| V43 | poppler returns a **non-null 1×1** image when it refuses an allocation | Z11a — every `isNull()` guard was inert |
| V44 | `kMinZoom` defeated fit modes; a 793-byte file allocated 2 GB | Z11g |
| V45 | Ctrl+I froze for minutes | Z11e — cached properties, 54 ms → 0 ms |
| V46 | a 1000-page print froze the app for 346 s | Z11h — modal progress + cancel; Z11j closed the re-entrancy that introduced |
| V47 | print-to-file overwrote the document being read | Z11e |
| V48 | "Current Page" and "Selection" printed the whole document | Z11e |
| V49 | the locked-document deref was broader than first proved | Z11a — `isOpen()`/`isLocked()` split; **295/295 encrypted mutants crashed before** |
| V51 | `flattenOutline` recursed unbounded | Z11a — depth cap 64 |

## Ruled — redaction, withdrawn

Seven findings (**V7, V8, V13, V19, V20, V29, V30**) are all one defect: the content-stream filter *approximates*
text layout instead of modelling it. It tests a show operator's origin rather
than its painted extent, never advances the text matrix across glyphs, and
drops the line advance carried by `'` and `"`.

Two reviewers working separately, on two different PDF engines, each confirmed
it reports success with the selected text still extractable.

**Ruled: withdrawn at Z11b.** Not shipped broken, not deleted. Unreachable from
the command overlay; the code and its reasoning are retained in the tree, and
MZ.md §13 records what returning it would require. V6 (source destruction) was
fixed anyway, because a withdrawn feature should still not be able to destroy
a file if someone reaches it.

## Ruled — where an elevated document may go

Recorded in MZ.md §13, because the three answers differ and the inconsistency
would otherwise look like an oversight:

- **redaction** — refused (predates the withdrawal)
- **print to a file** — refused at Z11g; print to a *printer* stays allowed
- **the clipboard** — **allowed, deliberately**. A selection is text already on
  screen; the clipboard is volatile and holds a selection, not a document.
  Refusing it would make the elevation path nearly useless while stopping
  nobody who can retype a line. The guarantee is that MERGEN will not write a
  file — not that an authenticated reader cannot use what they were shown.

## Rejected — measured, and the report was wrong

| # | claim | what the measurement showed |
|---|---|---|
| V35 | `selectedText()` line breaks wrong at every non-zero rotation | **Wrong.** `setRotation` already clears `m_words`, `m_links` *and* the selection when the angle changes (`pageview.cpp`, the `turned` branch). Word boxes are fetched per rotation and the reading-order heuristic runs in page space. No stale boxes exist to be wrong. |
| V16 | caches grow by **73.6 MB** over a thousand pages | **Magnitude wrong.** Measured by selecting across all 1000 pages of `large.pdf` twice: 3.7 MB unpruned, 2.5 MB pruned. Scrolling alone never fills the word cache at all — only selecting does. The prune was kept as a *bound*, and the commit message says so; the regression check asserts boundedness, not a difference, because with the documents in hand it cannot discriminate. |

## Partial — bounded, not eliminated

| # | finding | what was done, and what remains |
|---|---|---|
| V27 | privileged content comes to rest in several more places | Split three ways. Print-to-file was real — fixed at Z11g. The clipboard is real — **ruled allowed**, see above. The remaining named sites, the render cache and the search worker, already wipe (`document.cpp`, and the worker's end-of-pass wipe), so there was nothing to do there. |
| V50 | document-controlled geometry reaches `qRound()` unvalidated | `renderPage` refuses anything over 1.5 GiB and rejects poppler's 1×1 consolation prize, so the exploitable path is closed. A general "validate every geometric field on load" pass is **not** done; it is a design change, not a patch. |
| V52 | synchronous CPU exhaustion, quantified | The three measured cases are fixed (V18 compare, V45 properties, V46 print). Rendering itself is still synchronous on the GUI thread — that is the architecture, stated in MZ.md, not a defect introduced here. |
| — | clazy hygiene items *(not a numbered finding)* | not done. They are style findings with no behavioural consequence, and touching forty call sites the week of a freeze is the riskier choice. |
| — | `readElevated` nested event loop *(not a numbered finding)* | still a nested loop. It is now guarded (`m_busy`, `m_opening`, `QPointer`) and the crash it caused is fixed and tested, but the structure remains. |
| — | MZ.md §8 atomicity claim *(not a numbered finding)* | the *document* was corrected at Z11b to describe what the code does. The code was not changed to match the original claim. |

## Proven correct — do not "fix"

From the audit's own negative results, recorded so nobody re-opens them:

`rotateRect`/`unrotateRect` (290,400 cases, 0 mismatches) · `paintSearchHits`
(pixel-exact, all rotations) · `linkAt` (27/27) · the Esc chain (every
combination, three deep) · keyboard routing (no gaps) · the render cache
(bounded — three independent measurement methods) · `deeds` in `showCommands` ·
`m_focusBefore` and its `!now` guard · `mergen-open`'s descriptor validation ·
`document.cpp`'s `Goto`-only link filter · `addContentTokenFilter` as the API
choice · split content streams · encrypted and linearized handling · the
network guarantee (LD_PRELOAD interposer, **zero sockets**).

X3 lists sixteen load-bearing defences in full.

---

## The freeze question

The audit was commissioned to answer one question: *can this be frozen?*

At the time it reported, **no** — the answer in `RESUME.md` §5 stands as the
record of that moment. Eight commits later (Z11a–Z11h) every CRITICAL and HIGH
finding is either fixed, or ruled by withdrawing the feature that carried it.
What remains is listed above as partial, each with a stated reason.

One thing this pass did **not** do: a clean-chroot package build
(`extra-x86_64-build`). It needs root with a TTY. That is the last unverified
step before the tag is worth pushing.
