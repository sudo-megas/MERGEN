# H1 — adversarial audit of `src/redact.cpp`

Target: `<repo>/src/redact.cpp` (248 lines) and `<repo>/src/redact.h`.
Reference: `build/docs/MZ.md` §3, §5, §9 (Redaction), §10 Z10.
Method: read + empirical. Harness at `/tmp/audit/probe.cpp` links the real
`redact.cpp.o` from `build/CMakeFiles/mergen.dir/` and reproduces
`MainWindow::redactSelection()`'s input computation exactly (poppler
`textList()` word boxes → union → y-flip against `Poppler::Page::pageSizeF()` →
`Redact::run`). Adversarial PDFs hand-written with `/tmp/audit/mkpdf.py`.
Environment: qpdf 12.3.2, poppler-qt6 26.08.0.

**The question being answered:** can `Result.ok == true` be returned while the
text the reader asked to remove is still extractable from the written file?

**Answer: yes. Confirmed with a running reproduction.** See F1.

Status legend: `CONFIRMED` = reproduced by running the real code;
`READ-ONLY` = assessed by reading; `REFUTED` = checked and found not to be a bug.

---

## F1 — CRITICAL — `redact.cpp:184` + `redact.cpp:150` — reports success with the selected text fully intact and extractable

**CONFIRMED with a running reproduction.**

Two defects compose into a total failure of the guarantee MZ.md §5 exists to
make ("Redaction that only draws over content without removing it" is banned):

* `inside()` (redact.cpp:150-153) tests only the *origin* of the text object.
  A show operator whose origin lies outside the selected rectangle survives
  intact no matter how much of its text is rendered inside the rectangle.
  Since the text matrix is never advanced across shown glyphs (see F3), the
  origin of a whole line is the start of that line — so selecting anything but
  the first word of a line leaves that line's operator untouched.
* `stillThere()` (redact.cpp:183-187) only checks words of length >= 3:
  `if (word.size() >= 3 && text.contains(word))`. A selection made entirely of
  short tokens is never checked at all, so verification returns "clean"
  vacuously.

The `removed == 0` guard (redact.cpp:229) is the only thing standing between
these two, and it is satisfied by *any* removal anywhere in the rectangle — it
does not check that the removal had anything to do with the selected text.

### Reproduction (actually run)

`/tmp/audit/t1.pdf`, page content stream verbatim:

```
BT /F1 18 Tf 72 700 Td (Employee record - confidential) Tj ET
BT /F1 18 Tf 72 670 Td (Salary band 42) Tj ET
BT /F1 18 Tf 175 640 Td (k per annum, reviewed annually) Tj ET
```

The reader selects `42` and the `k` below it (drag across two lines — the
normal way anyone selects a figure and its unit). That is
`expected = "42 k"`, `area = x 173.05..193.07, y 636.27..682.92` in PDF user
space — computed by the real `MainWindow` code path, not by hand.

* Line 3's origin `(175, 640)` is inside the rectangle → dropped, `removed = 1`.
* Line 2's origin `(72, 670)` is outside → `(Salary band 42) Tj` survives whole.
* `expected` splits to `"42"` (2 chars) and `"k"` (1 char). Neither reaches the
  `size() >= 3` test, so `stillThere()` returns `false`.

Observed:

```
RESULT ok=TRUE  error=""
$ pdftotext t1out.pdf -
Employee record - confidential
Salary band 42
```

MERGEN tells the reader `Redacted copy written to t1out.pdf.` The page renders
as `Salary band ███` (verified with `pdftoppm`) — the exact black-box lie the
`qpdf` dependency was added to avoid (MZ.md §3). It is worse than doing
nothing, because the *other* line the reader partly selected did vanish, which
is visible confirmation to the reader that the tool worked.

### Minimal variant (also run) — one short word, no multi-line drag

`/tmp/audit/t2.pdf`:

```
BT /F1 18 Tf 72 700 Td (Employee record - confidential) Tj ET
BT /F1 18 Tf 72 670 Td (Salary band 42) Tj ET
BT /F1 18 Tf 180 672 Td ( ) Tj ET
```

Selecting only `42` (`expected = "42"`) gives `RESULT ok=TRUE` and
`pdftotext` still prints `Salary band 42`. The third line is a show operator
carrying nothing but a space; poppler does not turn it into a selectable word,
so the reader cannot see it exists, yet it is what satisfies `removed > 0`.

This is not a contrived shape. Short secrets are the common case for
redaction: a two-digit age, a room or ward number, a grade, initials, a
two-letter country or state code, a score, `"No"`/`"Yes"`.

### Severity

Critical. It is the one failure mode MZ.md §9 says the verification exists to
prevent ("the written file is deleted and the reader is told nothing was
saved"), and the code fails to deliver it.

### Fix direction

Two independent changes, both needed:

1. Verification must not have a length floor. Compare on the *whole* selected
   string (normalised for whitespace) as well as per word, and treat "no word
   was long enough to check" as a failure to verify, not as a pass. A
   verification step that can be satisfied vacuously is not a verification step.
   Better still, verify positionally: extract text from the redacted region of
   the output (`Poppler::Page::text(area)`) and require it to be empty, which
   removes the dependence on word length entirely.
2. `inside()` must test the *extent* of the shown text, not one point. The
   honest cheap version is to intersect the rectangle with the span from the
   operator's origin to the origin of the next positioning operator on the same
   line; the correct version needs glyph widths from the font. Failing both,
   the tool must refuse rather than report success — see also F2, where a whole
   line is deleted for the opposite reason.

---
## F2 — HIGH — `redact.cpp:99-106` — silently deletes text the reader did not select, and reports success

**CONFIRMED with a running reproduction.**

Because the whole show operator is dropped when its origin is inside the
rectangle, selecting the *first* word of a line deletes the entire line. The
reader is told the redaction succeeded and is never told that more was taken.

Reproduction: `/tmp/audit/t1.pdf` (content stream as in F1), select the single
word `Salary` on the line `Salary band 42`:

```
selected text  : "Salary"
RESULT ok=TRUE  error=""
$ pdftotext f2out.pdf -
Employee record - confidential
k per annum, reviewed annually
```

`band` and `42` are gone from the output although the reader selected neither.
On a real document this is unrecoverable loss in a file the reader believes is
a faithful copy minus one name. The black rectangle drawn over the gap is sized
to the *selection*, not to what was removed, so the page does not even show
where the missing text was.

Fix direction: same as F1(2) — the unit of removal must be the glyph run that
actually intersects the rectangle, not the whole operator. If the operator
cannot be split, the correct behaviour is to split it: re-emit the surviving
prefix and suffix as new show operators with an explicit `Tm`/`Td` for the
suffix, which is what a content-stream rewriter has to do to be honest.

---

## F3 — HIGH — `redact.cpp:53-110` — the text matrix is never advanced by shown text, so every operator in a text object reports the line's starting point

**CONFIRMED with a running reproduction.**

PDF requires `Tm` to advance by the horizontal displacement of every glyph
shown (`Tj`, `TJ`, `'`, `"`). `handleToken()` updates `m_text` for `BT`, `Tm`,
`Td`, `TD`, `T*`, `'` and `"` but never for the glyphs themselves — it cannot,
because it never reads `Tf`, the font's widths, `Tc`, `Tw` or `Tz`. This is the
mechanism behind both F1 and F2: consecutive show operators in one text object
all report the *same* origin.

Reproduction: `/tmp/audit/f3.pdf`:

```
BT /F1 18 Tf 72 700 Td (PUBLIC ) Tj (SECRETNAME) Tj ET
BT /F1 18 Tf 72 660 Td (tail line) Tj ET
```

Two independent wrong answers from the same defect:

* Select `SECRETNAME` (rendered at x 142..267) →
  `RESULT ok=false  error="Nothing removable was found in that area."`
  The second `Tj` reports origin `(72, 700)`, outside the rectangle. The tool
  cannot redact this text at all, and the message it gives is false: there was
  something removable there.
* Select `PUBLIC` (x 72..137) → `RESULT ok=TRUE`, and `pdftotext` on the output
  prints only `tail line`. `SECRETNAME` was deleted too, because its operator
  also reports origin `(72, 700)`.

This also means MZ.md §9's list of tracked operators — "`q` and `Q`, `cm`,
`BT`, `Tm`, `Td`, `TD`, `T*`, `TL`" — is not sufficient for the claim made
immediately before it, that the position of a show operator "has to be tracked
through the graphics and text matrices that preceded it". The matrices that
precede a show operator include the advance of every earlier show operator in
the same text object.

Fix direction: this needs font metrics. `QPDFObjectHandle` can reach the page's
`/Resources /Font` entries and their `/Widths`, `/FirstChar`, `/MissingWidth`
(and `/W` for Type0). Track `Tf` size, `Tc`, `Tw`, `Tz` and compute
`tx = ((w0 - Tj/1000) * Tfs + Tc + Tw) * Th` per PDF 32000-1 §9.4.4. If the
font cannot be measured (Type3, an embedded CID font with no usable widths),
the routine must refuse rather than guess — which is what `qpdf` was added to
the dependency list to make possible.

---
## F4 — HIGH — `redact.cpp:129`, `redact.cpp:142`, `redact.cpp:219-221` — all number formatting and parsing is locale-dependent, and Qt sets the locale

**CONFIRMED with a running reproduction.**

`QCoreApplication`'s constructor calls `setlocale(LC_ALL, "")` on Unix.
Verified directly:

```
before QApplication: LC_NUMERIC=C            to_string(72.5)=72.500000  stod("72.5")=72.5
after  QApplication: LC_NUMERIC=de_DE.UTF-8  to_string(72.5)=72,500000  stod("72.5")=72
```

`redact.cpp` uses `std::stod` (lines 129 and 142) to parse content-stream
operands and `std::to_string` (lines 219-221) to emit the cover rectangle.
Both follow `LC_NUMERIC`. Every reader on a European locale — the default for
most of Europe and Latin America — gets a different program.

### Half one: the black mark is never drawn (`redact.cpp:219-221`)

`/tmp/audit/t0.pdf` redacted under `LC_ALL=de_DE.UTF-8`, cover stream in the
written file:

```
q 0 0 0 rg 72,000000 645,032000 173,352000 22,200000 re f Q
```

`pdftoppm` on that file:

```
Syntax Error (493): Unknown operator ',000000'
Syntax Error (515): Too few (0) args to 're' operator
```

The routine returns `ok = TRUE`. The text really was removed in this case, but
the page has a silent blank where the mark should be. That is precisely what
MZ.md §9 says must not happen: "a black rectangle is drawn over the gap so the
page shows that something was taken rather than quietly closing over it." The
code fails to do what MZ.md claims, for every reader outside a
period-decimal locale. Rendered side by side, the C-locale output shows the
black bar and the de_DE output shows nothing.

### Half two: fractional coordinates are truncated (`redact.cpp:129`, `:142`)

`std::stod("72.5")` returns `72` under `LC_NUMERIC=de_DE`, because parsing stops
at the `.`. Every `cm`, `Tm`, `Td`, `TD` and `TL` operand loses its fractional
part, and real producers (TeX, Word, Chrome) emit fractional coordinates
constantly.

`/tmp/audit/f4.pdf`:

```
BT /F1 18 Tf 72.5 700.5 Td (SECRETWORD here) Tj ET
BT /F1 18 Tf 72 660 Td (tail line) Tj ET
```

Same selection, same rectangle, two locales:

```
C      : RESULT ok=TRUE   error=""
de_DE  : RESULT ok=false  error="Nothing removable was found in that area."
```

Truncation always moves a computed origin left and down, so it can also move an
operator's origin out of the rectangle while leaving another one in — which
composes directly with F1 into a fail-open on a locale where the C-locale build
would have been correct.

Fix direction: never use `std::stod`/`std::to_string` on PDF syntax. Parse with
`QPDFObjectHandle`'s own numeric accessors (the tokenizer already classifies
operands as `tt_integer`/`tt_real`, so
`QPDFObjectHandle::parse(token.getValue()).getNumericValue()` is available), or
`std::from_chars`, which is locale-independent by definition. Emit with
`QByteArray::number(v, 'f', 4)` or `std::to_chars`. Belt and braces: the cover
stream is machine syntax and should be built with a `QPDFObjectHandle` array
rather than string concatenation.

---
## F5 — HIGH — `redact.cpp:215` — text inside a Form XObject is never removed; only the verification step stops the lie, and F1's hole lets it through

**CONFIRMED with a running reproduction. Suspicion 3, partly confirmed and partly refuted.**

`addContentTokenFilter` is attached to the page's content stream only. The
filter never descends into `/XObject … Do`, so text drawn by a Form XObject is
never removed. Confirmed.

The *detection* half of the suspicion is refuted: poppler's
`Poppler::Page::text()` runs the full content interpreter and does descend into
form XObjects, so `stillThere()` does see that text.
`/tmp/audit/fa.pdf` — page content `q 1 0 0 1 0 0 cm /Fm0 Do Q` plus a
space-only show operator as a removal trigger, `/Fm0` containing
`BT /F1 18 Tf 100 700 Td (SECRETINFORM) Tj ET` — gives:

```
RESULT ok=false  error="The text is still in the rewritten file, so it was discarded. Nothing was saved."
```

and the output file is deleted. Correct refusal.

But that is the *only* thing protecting form XObjects, and F1 punches through
it. `/tmp/audit/fb.pdf`, identical but with the form XObject drawing `(42)`:

```
selected text  : "42"
RESULT ok=TRUE  error=""
$ pdftotext fbout.pdf -
42
```

Second independent fail-open, and this one needs no unusual content stream at
all — form XObjects are how every `\includegraphics`, every letterhead, every
stamped template and every `pdftk`-merged page is built.

Fix direction: recurse. `QPDFPageObjectHelper::getFormXObjects()` enumerates
them and `addContentTokenFilter` is documented to work on form XObjects too, so
the filter can be attached to each — with the XObject's own `/Matrix` and the
CTM at the `Do` composed into the filter's starting CTM, and a guard against
XObjects shared between pages (rewriting one edits every page that draws it).
Where recursion is not possible, refuse.

---

## F6 — REFUTED — nested `q`/`Q` inside `BT`/`ET` composes correctly

**Suspicion 2: checked and disproved for the ordinary shape.**

`/tmp/audit/fc.pdf`:

```
BT /F1 18 Tf
q 1 0 0 1 300 0 cm
72 700 Td (SHIFTEDSECRET) Tj
Q
ET
```

Poppler renders the text at x=372; the filter computes `Tm × CTM` origin
`(372, 700)`, the rectangle is x 372..522, and the text is removed —
`RESULT ok=TRUE`, output text empty. The matrix algebra in `Matrix::times`
(redact.cpp:33-38) is correct row-vector composition, `cm` is applied as
`cm × CTM` (redact.cpp:75), `Td`/`T*` as `translate × Tlm` (redact.cpp:88, :91),
and `BT` resets both text matrices (redact.cpp:76-78) — all as PDF 32000-1
§8.4.4 and §9.4.2 require. `m_text`/`m_line` are correctly *not* pushed by `q`,
since the text matrices are not part of the graphics state.

One real defect in this area, F7 below.

---

## F7 — MEDIUM — `redact.cpp:161` — `TL` (leading) is graphics state but is not saved and restored by `q`/`Q`

**CONFIRMED with a running reproduction.**

`m_leading` sits outside `m_stack`, so `Q` does not restore it. PDF 32000-1
Table 52 lists leading as a text-state parameter, and text state is part of the
graphics state saved by `q`.

`/tmp/audit/fd.pdf`:

```
BT /F1 18 Tf 1 0 0 1 72 700 Tm 20 TL
q 40 TL Q
T* (SECRETLEADING) Tj
ET
```

Poppler draws the text at baseline 680 (leading correctly restored to 20). The
filter carries `m_leading = 40` past the `Q` and computes the origin at
baseline 660, twenty points below the rectangle:

```
PDF area : x=72.000 y=676.274 w=152.028 h=16.650
RESULT ok=false  error="Nothing removable was found in that area."
```

The redaction is refused on text that is plainly inside the selection. In the
mirror case — a `q … TL … Q` that *lowers* the leading — the miscomputed origin
lands inside the rectangle and an unrelated line is deleted instead, which
composes with F1 into a fail-open.

Fix direction: the stack entry must be the whole graphics state, not just the
CTM: `struct GState { Matrix ctm; double leading; double charSpace; double
wordSpace; double hscale; double rise; ... }`. The same applies to `Tc`, `Tw`,
`Tz`, `Ts` and `Tf` once F3 makes them load-bearing.

---
## F8 — CRITICAL — `mainwindow.cpp:966` vs `redact.cpp:150` — the rectangle is computed in poppler's *display* space and used as PDF *user* space; `/Rotate` and `/CropBox` break it, and it fails open

**CONFIRMED with a running reproduction.**

`redact.h` documents `area` as "in PDF user space — points, origin at the
bottom-left of the page". What is actually passed
(`mainwindow.cpp:966`) is derived from `Poppler::Page::textList()` boxes
flipped against `Poppler::Page::pageSizeF()`. Poppler's page space is the
**CropBox**, with `/Rotate` already applied. Those are three different spaces
whenever a page has a `/Rotate` or a `/CropBox` that is not the `/MediaBox`.
Neither `redact.cpp` nor `mainwindow.cpp` reads `/CropBox` or `/Rotate` at all.

Measured, same content stream in three page dictionaries
(`BT /F1 18 Tf 72 700 Td (SECRETROTATED) Tj ET`, `/tmp/audit/{rot90,crop,cropx}.pdf`):

| page dict | poppler page size | rectangle handed to `Redact::run` | true origin | result |
|---|---|---|---|---|
| `/Rotate 90` | 792 x 612 | x 696.3..712.9, y 381.0..540.0 | (72, 700) | refused |
| `/CropBox [0 300 612 792]` | 612 x 492 | x 72..231, y 396.3..412.9 | (72, 700) | refused |
| `/CropBox [50 300 612 792]` | 562 x 492 | x 22..181, y 396.3..412.9 | (72, 700) | refused |

The rectangle is off by exactly the CropBox origin, and transposed under
`/Rotate`. Both are ordinary: `/Rotate` on any scanned or landscape page, a
non-MediaBox `/CropBox` on anything trimmed, and both are what
`pdfcrop`, scanners and imposition tools emit.

### It fails open, not merely closed

`/tmp/audit/fh.pdf` — a `/CropBox [0 300 612 792]` page:

```
BT /F1 18 Tf 72 700 Td (Case ref 42) Tj ET
BT /F1 18 Tf 150 400 Td (Unrelated paragraph that must survive) Tj ET
```

Selecting `42` on the first line gives a rectangle at y 396..413 — 300 points
low, landing on the second line, whose origin `(150, 400)` is inside it.

```
selected text  : "42"
RESULT ok=TRUE  error=""
$ pdftotext fhout.pdf -
Case ref 42
```

Rendered side by side, the output still shows `Case ref 42` untouched, the
unrelated paragraph is **gone**, and the black mark is painted 300 points away
from anything the reader selected. MERGEN reports
`Redacted copy written to fhout.pdf.`

So on a cropped page this routine can leave the secret in place, destroy an
unrelated paragraph, and report success — all at once.

Fix direction: `area` must be converted from poppler display space to PDF user
space before it leaves `MainWindow`, or (better) `Redact::run` should take the
selection in poppler's terms and do the conversion itself using the page's own
`/MediaBox`, `/CropBox` and `/Rotate` read through `QPDFPageObjectHelper`
(`getMediaBox()`, `getCropBox()`, `getAttribute("/Rotate", true)`). Until then,
the routine must at minimum detect `/Rotate != 0` or `CropBox != MediaBox` and
refuse, since it cannot place the rectangle.

---

## F9 — REFUTED — inline images, `/Contents` arrays, object streams, linearization and encryption

**Suspicions 6 (inline images), 7 (multiple content streams) and 8: checked, no defect found.**

* **Inline images.** `/tmp/audit/fe.pdf` carries
  `BI /W 16 /H 1 /CS /G /BPC 8 ID 1 0 0 1 300 0 cm EI` — sixteen image bytes
  chosen to spell a `cm` operator. qpdf's `Pl_QPDFTokenizer` delivers the image
  data as one `tt_inline_image` token, the poisoned `cm` is never executed, the
  following `(SECRETAFTERIMAGE) Tj` is still measured at `(72, 700)` and is
  removed correctly, and the image data survives byte-intact in the output.
  The `tt_inline_image` token does land in `m_operands` (it is neither
  `tt_word`, `tt_space` nor `tt_comment`, redact.cpp:55-61), but the following
  `EI` is a `tt_word` and clears it, so the pollution is bounded to one operator
  that takes no numeric operands. No exploit found.
* **`/Contents` arrays.** `QPDFPageObjectHelper.hh:275-277` documents that
  `addContentTokenFilter` coalesces an array into a single stream first, so
  there is no cross-stream state problem by construction. Verified with
  `/tmp/audit/ff.pdf`, whose `/Contents [4 0 R 6 0 R 7 0 R]` splits one text
  object across three streams with the load-bearing `cm` in the first:
  `(SPLITSECRET)` at x=172 is measured and removed correctly, `RESULT ok=TRUE`.
* **Object streams / compressed streams** (`qpdf --object-streams=generate
  --stream-data=compress`): works, `ok=TRUE`, correct output.
* **Linearized source** (`qpdf --linearize`): works, `ok=TRUE`. The output is
  not re-linearized, which is a property loss and not a correctness one.
* **Encrypted, empty user password** (`--encrypt --user-password= --bits=256
  --extract=n --print=none`): works, and `QPDFWriter` preserves the encryption
  and permissions on the output (`R = 6  P = -2072`). Good — the redacted copy
  does not silently lose the source's protection.
* **Encrypted, real user password:** `Redact::run` calls `processFile` with no
  password and fails closed:
  `The document could not be rewritten: t0_encuser.pdf: invalid password`.
  See F13 — the outcome is safe but undocumented.
* **Guards:** source == destination →
  `"Redaction never writes over the document it read."`; out-of-range page →
  `"That page is not in the document."`. Both correct.

---
## F10 — CRITICAL — `redact.cpp:98-106` — dropping `'` or `"` discards their line-advance side effect, shifting the rest of the text object up one line; the next line ends up *underneath the black rectangle*

**CONFIRMED with a running reproduction.**

`'` and `"` are not pure show operators: each performs `T*` first. The filter
models that correctly for its own state (redact.cpp:93-96) but, when it drops
the operator, it drops the `T*` along with it — so the *rendered* output never
performs the move. Every subsequent line in that text object shifts up by one
leading.

`/tmp/audit/fi.pdf`:

```
BT /F1 18 Tf 20 TL 1 0 0 1 72 700 Tm
(line one) '
(SECRETQUOTE) '
(line three) '
(line four) '
ET
```

Redacting `SECRETQUOTE`:

```
RESULT ok=TRUE  error=""
```

Word positions before → after, measured with poppler on both files:

| | source y | output y |
|---|---|---|
| `line one` | 99.08 | 99.08 |
| `SECRETQUOTE` | 119.08 | removed |
| `line three` | 139.08 | **119.08** |
| `line four` | 159.08 | 139.08 |

`line three` has moved into the slot the black rectangle covers. Rendered, the
output reads:

```
line one
███████████████
line four
```

`pdftotext` on the same file prints `line one / line three / line four`.

**That is the exact lie the project exists to refuse:** a black rectangle with
live, extractable text underneath it, produced by MERGEN's own redaction path
and reported as success (MZ.md §5 "Redaction that only draws over content
without removing it"; MZ.md §3 "the text sits underneath, selectable, and every
tool that ships it that way is handing the reader a false guarantee").

The verification step cannot catch it: `line three` was never part of the
selection, so `stillThere()` is not looking for it.

Fix direction: when dropping `'`, `"`, or any operator with a positioning side
effect, emit the side effect on its own — replace `(x) '` with `T*`, and
`aw ac (x) "` with `aw Tw ac Tc T*`. More generally, only the *glyph-painting*
part of an operator may be discarded; its state changes must be preserved.

---

## F11 — HIGH — `redact.cpp:215` — a content stream shared by two pages is rewritten for both, destroying text on a page the reader never selected

**CONFIRMED with a running reproduction.**

The filter is attached to a `QPDFObjectHandle` — the stream object — not to a
page. Two pages whose `/Contents` point at the same stream (page templates,
repeated boilerplate, `pdftk`-style imposition) are both rewritten.

`/tmp/audit/fj.pdf`: objects 3 and 6 are two pages, both with
`/Contents 4 0 R`, the stream drawing `(SHAREDSECRET)`.

```
RESULT ok=TRUE  error=""
$ pdftotext -f 2 -l 2 fjout.pdf -      # the page that was NOT redacted
(empty)
```

Page 2 lost its text and did not even get the black mark, because
`addPageContents` was called on the target page only. The reader is told the
redaction succeeded and is never told a second page was altered.

Fix direction: check whether the page's content streams are referenced from
more than one page (walk `getAllPages()` and compare object IDs) and either
copy the stream for the target page before filtering
(`QPDFObjectHandle::shallowCopy` + reassign `/Contents`) or refuse. The same
guard is needed for the form XObject recursion proposed in F5.

---

## F12 — MEDIUM — `redact.cpp:183-187` — a word that appears anywhere else on the page can never be redacted

**CONFIRMED with a running reproduction.**

`stillThere()` searches the whole page for the selected word. A correct
redaction of one occurrence is therefore always reported as a failure, and the
correctly-redacted output is deleted.

`/tmp/audit/fk.pdf`:

```
BT /F1 18 Tf 72 700 Td (Lovelace) Tj ET
BT /F1 18 Tf 72 600 Td (Lovelace again) Tj ET
```

Redacting the first `Lovelace`:

```
RESULT ok=false  error="The text is still in the rewritten file, so it was discarded. Nothing was saved."
```

Removing one occurrence of a name from a page where it also appears in a header,
a footer, a signature block or a second paragraph is the single most common
redaction there is, and it is impossible here. The message is also misleading:
the removal did work.

This matters for security as well as usability. A reader who hits it repeatedly
learns that MERGEN's redaction "doesn't work", and stops trusting the one
message that is load-bearing — the refusal in F1's class of case.

Fix direction: verify positionally rather than by page-wide substring — extract
text from the redacted rectangle in the output (`Poppler::Page::text(area)`, or
a small inset of it) and require it to be empty, plus require the *count* of
occurrences on the page to have dropped by the number redacted. Both are strictly
stronger than the current test and neither has the length floor from F1.

---
## F13 — MEDIUM — `redact.cpp:151` — text rise (`Ts`) is not applied to the origin

**CONFIRMED with a running reproduction. Suspicion 4, the `Ts` part.**

The text rendering matrix is
`[Tfs·Th 0 0 Tfs 0 Ts] × Tm × CTM` (PDF 32000-1 §9.4.4). Its origin is
`(0, Ts)` mapped through `Tm × CTM`, not `(0, 0)`. `inside()` uses
`m_text.times(m_ctm).origin()`, i.e. `Ts = 0` always.

`/tmp/audit/fts.pdf`:

```
BT /F1 18 Tf 72 700 Td 30 Ts (SECRETRISE) Tj ET
```

Poppler places the text at baseline 730; the filter computes `(72, 700)`, 30
points below the rectangle:

```
PDF area : x=72.000 y=726.274 w=115.020 h=16.650
RESULT ok=false  error="Nothing removable was found in that area."
```

Superscripts, footnote markers and chemical formulae all use `Ts`, and a
negative `Ts` moves the computed origin the other way — into a rectangle it does
not belong in, which composes with F1.

**The rest of suspicion 4 is refuted as stated.** `Tz` (horizontal scaling) does
not move the text-object origin at all — it scales `Th` in the `a` cell, whose
`e` component is zero — and `Tc`, `Tw` and the size from `Tf` affect only the
glyph *advance*. None of them can move the origin. They matter, but they matter
as F3 (the advance is never computed), not as a separate defect.

Fix direction: fold `Ts` into the origin (`Matrix{{1,0,0,1,0,rise}}.times(m_text).times(m_ctm)`),
and put `Ts` on the graphics-state stack per F7.

---

## F14 — HIGH — `redact.cpp:98` — a `TJ` array is one operator with one origin, so it is all-or-nothing

**CONFIRMED with a running reproduction. Suspicion 1, second half.**

`/tmp/audit/ftj.pdf`:

```
BT /F1 18 Tf 72 700 Td [(PUBLICPREFIX) -300 (SECRETSUFFIX)] TJ ET
```

Poppler reports two selectable words, at x=72 and x=207.43. The content stream
has one operator, with one origin at `(72, 700)`.

* Select `SECRETSUFFIX` → `RESULT ok=false  "Nothing removable was found in that area."`
* Select `PUBLICPREFIX` → `RESULT ok=TRUE`, and the output page is empty —
  `SECRETSUFFIX` went with it.

This is not an exotic construction: `TJ` with kerning offsets is how essentially
every PDF produced by TeX, Word, LibreOffice or a browser lays out a line of
text. On such documents this routine can only ever redact a whole line, and only
when the reader's selection starts at the line's first glyph.

Fix direction: `TJ` must be rewritten element-wise. The array's own numbers give
the inter-element displacements (`-Tj/1000 · Tfs · Th`), so the position of each
string element is computable *without* font widths as long as the glyph widths
within each element are known; combined with F3's width lookup, individual
elements can be dropped and the surrounding elements' offsets adjusted to keep
the rest of the line in place.

---

## F15 — MEDIUM — `redact.cpp:222` — the cover rectangle inherits the page's leftover graphics state, so the mark can land anywhere or vanish

**CONFIRMED with a running reproduction.**

`addPageContents(stream, false)` appends (confirmed against
`QPDFPageObjectHelper.hh:219-223`), which correctly puts the mark on top. But
the appended stream begins with `q`, not with anything that resets state, so it
draws under whatever CTM, clip path and colour the page's own content stream
left in force. A page that does not close its `q` — common enough that
every renderer tolerates it — displaces the mark.

`/tmp/audit/fq.pdf`:

```
q 0.5 0 0 0.5 0 0 cm
BT /F1 36 Tf 144 1400 Td (SCALEDSECRET) Tj ET
```

The removal is correct (the filter tracks the `cm`), and the emitted cover
stream carries the right user-space numbers:

```
q 0 0 0 rg 72.000000 696.274000 145.026000 16.650000 re f Q
```

but the black pixels in the rendered output measure to `x 36..109, y 436..444`
instead of `x 72..217, y 79..96` — half size, in the wrong place. A page left
inside a clipping path loses the mark entirely.

Fix direction: emit the cover as a leading `Q`-balanced block —
`QPDFPageObjectHelper::addPageContents` a *prefix* stream containing `q` and a
suffix containing `Q` around the original content (qpdf's
`wrapContentsInFormXObject`/`pushInheritedAttributesToPage` idiom), or simply
prepend `n` copies of `Q` matched to the imbalance. The robust version is to
wrap the original content in a form XObject and then draw the mark in a clean
state, which qpdf supports directly.

---

## F16 — LOW — `redact.cpp:233-236` — a failed rewrite leaves a truncated file at the destination

**READ-ONLY.**

`QPDFWriter`'s constructor (redact.cpp:224) creates and truncates the
destination. If `write()` throws — a corrupt object anywhere in the document,
not only on the target page — the `catch` at line 233 returns an error message
and never calls `QFile::remove(destination)`, unlike the two other failure paths
(lines 230 and 240). The reader is told "The document could not be rewritten"
and is left with a truncated PDF at the path they chose, which the save dialog
may well have had them overwrite an existing file to reach.

Fix direction: remove the destination in the `catch` as well, or write to a
temporary in the same directory and rename into place only on success — which is
the pattern `portals.toml` already uses per MZ.md §8.

---

## F17 — LOW — `redact.cpp:193` and MZ.md §9 — a third refusal is undocumented

**CONFIRMED by running.**

MZ.md §9 says "Two things it declines rather than half-does" and names a
selection spanning two pages and a document opened through the elevation helper.
There is a third: a document with a user password.
`Redact::run` calls `processFile` with no password (redact.cpp:202) and there is
nowhere for the reader's password to be passed:

```
$ ./direct t0_encuser.pdf out.pdf 0 72 645 174 23 "SECRET NAME"
RESULT ok=false error="The document could not be rewritten: t0_encuser.pdf: invalid password"
```

Safe, but the reader gets a qpdf diagnostic instead of a sentence, and MZ.md's
count of what redaction declines is wrong. (MZ.md §5 bans storing passwords, so
the fix is to pass the in-memory password through to `processFile`, not to
persist it; or to say so plainly in §9 and in the message.)

---
## F18 — CRITICAL — `redact.cpp:195` — the "never write over the source" guard compares path *spellings*, so a symlink or hard link lets redaction destroy the reader's original document

**CONFIRMED with a running reproduction.**

```cpp
if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath()) {
    return {false, QObject::tr("Redaction never writes over the document it read.")};
}
```

`absoluteFilePath()` cleans `.` and `..` but does **not** resolve symbolic
links, and no path comparison can catch a hard link. Two different spellings of
the same inode pass the guard, and `QPDFWriter` (redact.cpp:224) then truncates
and rewrites the file `QPDF` is still lazily reading from.

### Symlink, small file — source silently replaced

```
$ ln -sf /tmp/audit/orig.pdf /tmp/audit/alias.pdf
$ md5sum orig.pdf                       0d52de65db0a33379ef849d56da655aa
$ ./direct /tmp/audit/orig.pdf /tmp/audit/alias.pdf 0 72 645 174 23 "SECRET NAME"
RESULT ok=TRUE error=""
$ md5sum orig.pdf                       344a7aca38b811df67c0d3a6cb79e434
```

### Symlink, 10-page 100 KB file — source irrecoverably destroyed

qpdf reads objects on demand during `write()`, so on any document too large to
have been fully parsed up front, the write races its own reads.

```
$ ls -la bigorig.pdf                    102801 bytes, 10 pages, text on every page
$ ./direct /tmp/audit/bigorig.pdf /tmp/audit/bigalias.pdf 3 72 690 174 23 "SECRET NAME"
RESULT ok=false error="Nothing removable was found in that area."
$ ls -la bigorig.pdf                    2837 bytes
$ pdftotext bigorig.pdf -               (nothing — every content stream truncated)
```

The reader's original document is gone, every page blank, and the only thing
they are told is *"Nothing removable was found in that area."* The failure path
then calls `QFile::remove(destination)` (redact.cpp:230), which deletes the
symlink as well, so even the path they used disappears.

### Hard link — same, and reported as success

```
$ ln hsrc.pdf hdst.pdf
$ ./direct /tmp/audit/hsrc.pdf /tmp/audit/hdst.pdf 0 72 645 174 23 "SECRET NAME"
RESULT ok=TRUE error=""
$ md5sum hsrc.pdf     changed — the source now holds the redacted content
```

### Why this is reachable, not theoretical

`MainWindow::redactSelection` (mainwindow.cpp:955-960) opens a save dialog
defaulting to `<basename>-redacted.pdf` beside the source. A reader who opened
the document through a symlink — `~/current.pdf -> ~/archive/2026/report.pdf`,
a syncthing or Nextcloud link farm, a `latest.pdf` pointer, anything under a
bind mount — and then saves next to the real file, or navigates to it in the
dialog, hits this. MERGEN's own recent-files list stores whatever path was
passed on the command line, so the two spellings genuinely coexist in normal use.

### What it violates

* MZ.md §5 DO-NOT: "**Editing a PDF in place** — every write produces a new file."
* MZ.md §8: "Redaction writes a new PDF to a path the reader chooses. It never
  modifies the open document, and the open document is never the destination."
* `redact.h:19`: "Nothing is written in place. The source is never the destination."

Fix direction: compare identity, not spelling. `QFileInfo::canonicalFilePath()`
handles symlinks; hard links need `stat(2)` and a `(st_dev, st_ino)` comparison,
which is the only correct test on Linux. Independently — and this is the change
that makes the whole class impossible — write to a temporary file in the
destination's directory and `rename()` into place only after `stillThere()` has
passed. That is the same atomic-write discipline MZ.md §8 already requires of
`portals.toml`, and it also fixes F16.

---
## F19 — HIGH — `redact.cpp:180` — the redacted text survives in metadata, outline titles and annotations, and the verification never looks there

**CONFIRMED with a running reproduction.**

`stillThere()` checks `Poppler::Page::text()` — the page's *rendered* text and
nothing else. Everything else in the file that can carry the same string is
untouched by the filter and unexamined by the verification.

`/tmp/audit/meta.pdf` draws `SECRETNAME` on the page and also carries it in
`/Info /Title`, `/Info /Author`, `/Info /Keywords`, an outline entry `/Title`
and a `/Text` annotation's `/Contents`. Redacting the on-page occurrence:

```
selected text  : "SECRETNAME"
RESULT ok=TRUE  error=""

$ pdftotext metaout.pdf -
public tail                       <- the page really was redacted

$ pdfinfo metaout.pdf
Title:      Dossier on SECRETNAME
Keywords:   SECRETNAME
Author:     SECRETNAME

$ qpdf --qdf metaout.pdf - | grep -c SECRETNAME
5
  /Author (SECRETNAME)
  /Keywords (SECRETNAME)
  /Title (Dossier on SECRETNAME)
  /Title (Chapter about SECRETNAME)
  /Contents (Note: SECRETNAME lives here)
```

The reader is told `Redacted copy written to metaout.pdf.` and the very first
thing any recipient's PDF viewer shows them — the window title — is the name
that was redacted.

This is the honest boundary case in this report, so state it precisely: MZ.md §4
scopes the *removal* to "the content stream", and `redact.h` says the same. But
MZ.md §9 scopes the *verification* to no such thing — "the result is verified by
searching the output for the removed text" — and the message the reader gets
carries no caveat at all. Under the standard this feature sets for itself ("a
redaction tool that cannot demonstrate the text is gone is the kind that ships
the lie this project added a dependency specifically to avoid"), leaving the name
in `/Info /Title` is shipping the lie.

Fix direction: two options, and the choice is a ruling for MZ.md, not a code
decision. Either (a) extend the removal — scrub `/Info`, XMP `/Metadata`,
outline `/Title`s, annotation `/Contents` and form field values of the redacted
string, which qpdf can do directly; or (b) keep the content-stream scope and make
the verification honest about it — scan the whole written file for the string and,
where it survives outside the page content, tell the reader exactly where, rather
than reporting unqualified success. Either way MZ.md §9 needs to say which.

---
## F20 — real-world confirmation that F1/F3/F14 are not artefacts of hand-written test files

**CONFIRMED by running against a third-party PDF.**

`/usr/share/cups/data/form_english.pdf` — produced by LibreOffice 4.0, shipped
with CUPS, chosen as the first real PDF to hand. Its heading is a single `TJ`:

```
BT
2 Tr 1.2 w
89.9 742.1 Td /F1 36 Tf[<01>19<02>42<03040506>1<0708090a0b>-1<02>83<0106>1<0907>]TJ
ET
```

One operator, one origin at `(89.9, 742.1)`, fractional coordinates, glyph
positions carried entirely in the kerning array.

* Redact `INFORMATION` (the second word of that heading, x 201..483):
  `RESULT ok=false  "Nothing removable was found in that area."`
  A word in the middle of a line cannot be redacted at all.
* Redact `TASK` (the first word, x 89.9):
  `RESULT ok=TRUE`, and the output's first text line is now `Task #:` — the
  whole heading, `INFORMATION` included, was deleted.

Two of the three shapes in this report reproduce on the first real document
tried, and F4's fractional-coordinate hazard is present in the same three lines.

---

## Summary — ranked

| # | Severity | Where | What | Evidence |
|---|---|---|---|---|
| F1 | **critical** | redact.cpp:150, :184 | Reports success with the selected text intact and extractable — origin-only hit test plus a verification that skips words shorter than 3 characters | reproduced |
| F8 | **critical** | mainwindow.cpp:966 / redact.cpp:150 | Rectangle is in poppler display space, used as PDF user space; `/Rotate` and `/CropBox` misplace it. Reproduced leaving the secret intact, destroying an unrelated paragraph, and reporting success | reproduced |
| F10 | **critical** | redact.cpp:98-106 | Dropping `'`/`"` discards their line advance; the following line slides up **underneath the black rectangle** and stays extractable | reproduced |
| F18 | **critical** | redact.cpp:195 | Source/destination guard compares path spellings; a symlink or hard link lets the write destroy the reader's original document (100 KB / 10-page file reduced to 2.8 KB of blank pages) | reproduced |
| F2 | high | redact.cpp:99-106 | Deletes text the reader did not select, silently, and reports success | reproduced |
| F3 | high | redact.cpp:53-110 | Text matrix never advanced across shown glyphs — every operator in a text object reports the line's start | reproduced |
| F4 | high | redact.cpp:129, :142, :219 | `std::stod`/`std::to_string` follow `LC_NUMERIC`, which Qt sets; the cover rectangle becomes invalid PDF and fractional coordinates are truncated | reproduced |
| F5 | high | redact.cpp:215 | Form XObject text is never removed; only verification catches it, and F1 gets past verification | reproduced |
| F11 | high | redact.cpp:215 | A content stream shared by two pages is rewritten for both | reproduced |
| F14 | high | redact.cpp:98 | `TJ` is all-or-nothing — one operator, one origin | reproduced |
| F19 | high | redact.cpp:180 | The redacted string survives in `/Info`, outline titles and annotations; verification never looks | reproduced |
| F7 | medium | redact.cpp:161 | `TL` is graphics state but is not saved/restored by `q`/`Q` | reproduced |
| F12 | medium | redact.cpp:183 | A word appearing elsewhere on the page makes correct redaction impossible | reproduced |
| F13 | medium | redact.cpp:151 | Text rise (`Ts`) not applied to the origin | reproduced |
| F15 | medium | redact.cpp:222 | Cover rectangle inherits the page's leftover graphics state | reproduced |
| F16 | low | redact.cpp:233 | A failed rewrite leaves a truncated file at the destination | read-only |
| F17 | low | redact.cpp:193 | Password-protected sources are a third undocumented refusal | reproduced |

Refuted: F6 (nested `q`/`Q` inside `BT` — correct), F9 (inline images,
`/Contents` arrays, object streams, linearization, encryption — all correct),
and the `Tz`/`Tc`/`Tw`/`Tf` half of suspicion 4, which cannot move the origin
and folds into F3.

## Answer to the question that matters

**Yes, and by four independent routes**, three of which need no unusual input:

1. F1 — any selection whose words are all shorter than three characters, or any
   mixed selection where the long words happen to be removed and a short one is
   not.
2. F8 — any page with a `/CropBox` that is not the `/MediaBox`, or a `/Rotate`.
3. F10 — any text object using `'` or `"`, where the line after the redacted one
   slides under the black rectangle.
4. F5 combined with F1 — a short secret drawn by a form XObject.

The `removed == 0` check at redact.cpp:229 is what makes all of these reachable:
it establishes only that *something* was removed *somewhere* in the rectangle,
and is then treated as evidence that the selected text was what went.

## Smaller notes and nits

* `redact.cpp:124-133`, `:135-148` — `operand()` and `numbers()` read back from
  the end of `m_operands`, which is the correct way to handle an operator
  preceded by extra tokens. But an operator given *too few* operands silently
  yields `0.0` / the identity matrix rather than signalling that the stream was
  not understood. A content-stream walker that quietly guesses is the wrong shape
  for a security routine; a parse it cannot follow should make it refuse.
  `numbers()`'s per-element catch (`out.m[i] = (i == 0 || i == 3) ? 1.0 : 0.0`)
  can build a matrix that is half the file's and half the identity — worse than
  either.
* `redact.cpp:55-61` — `tt_bad` tokens are treated as operands. A malformed
  stream therefore feeds the position maths instead of stopping it. Same remark.
* `redact.cpp:102` — `*m_removed` counts *dropped operators*, not "occurrences of
  the selected text removed". The name and the use at line 229 both read as the
  latter. Renaming it `m_dropped` would at least stop the code asserting
  something it does not know.
* `redact.cpp:225` — `writer.setStaticID(false)` is already `QPDFWriter`'s
  default; the line is a no-op. Harmless, but it reads as if it were doing
  something.
* `redact.cpp:171` — `Poppler::Document::load` failure and `isLocked()` both
  return `true` from `stillThere()`, i.e. "cannot be shown clean, so not
  claimed". That is the right default and the comment says so. Good.
* `redact.cpp:195` and `:206` — the source/destination and page-range guards
  both fail closed with clear sentences. Good, apart from F18.
* MZ.md §9 — the operator list ("`q` and `Q`, `cm`, `BT`, `Tm`, `Td`, `TD`,
  `T*`, `TL`") is what the code tracks, so the code matches the document there.
  What does not match is the sentence just before it: tracking "the graphics and
  text matrices that preceded" a show operator requires the glyph advances too
  (F3), and the verification MZ.md describes without qualification has a length
  floor (F1) and a page-text-only scope (F19). MZ.md §9's "Two things it
  declines" is three (F17).
