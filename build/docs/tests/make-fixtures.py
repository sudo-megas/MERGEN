#!/usr/bin/env python3
"""Write every PDF the acceptance suites open.

The suites take their documents from the working directory. Those documents used
to be untracked files in the repository root, produced by throwaway scripts and
described in BUILD.md as "see the git history" — which was not true, because the
scripts were never in it. Deleting the PDFs made all eleven suites unrunnable,
which is how the gap was found.

Run this from wherever the suites will run:

    python3 build/docs/tests/make-fixtures.py

Everything is written by hand as raw PDF, so this needs no library and no
network. `locked.pdf` is the one exception and needs `qpdf` on the path; without
it that file is skipped and the two locked-document checks in z11 skip with it.
"""

import os
import shutil
import subprocess
import sys
import zlib


def build(objects, extra_trailer=""):
    """Assemble numbered objects into a PDF with a correct xref table."""
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(out))
        if isinstance(body, str):
            body = body.encode("latin-1")
        out += f"{number} 0 obj\n".encode() + body + b"\nendobj\n"
    start = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode()
    for offset in offsets:
        out += f"{offset:010d} 00000 n \n".encode()
    out += (
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R {extra_trailer}>>\n"
        f"startxref\n{start}\n%%EOF\n"
    ).encode()
    return bytes(out)


def stream(data, extra=""):
    if isinstance(data, str):
        data = data.encode("latin-1")
    return f"<< /Length {len(data)} {extra}>>\nstream\n".encode() + data + b"\nendstream"


A4 = "[0 0 595 842]"


def text_page(lines, size=16, start_y=700, leading=40):
    """A content stream drawing one line of text per entry."""
    body = ["BT", f"/F1 {size} Tf", f"1 0 0 1 60 {start_y} Tm", f"{leading} TL"]
    for line in lines:
        body.append(f"({line}) Tj T*")
    body.append("ET")
    return "\n".join(body)


def simple(path, pages, extra_page_keys=lambda i: "", extra_objects=(),
           catalog_extra="", first_object=6):
    """A document whose pages all share one font and differ only in content."""
    contents_first = first_object
    objects = [
        f"<< /Type /Catalog /Pages 2 0 R {catalog_extra}>>",
        "<< /Type /Pages /Kids [%s] /Count %d >>"
        % (" ".join(f"{3 + i} 0 R" for i in range(len(pages))), len(pages)),
    ]
    for i, _ in enumerate(pages):
        objects.append(
            f"<< /Type /Page /Parent 2 0 R /MediaBox {A4} "
            f"/Resources << /Font << /F1 {contents_first + len(pages)} 0 R >> >> "
            f"/Contents {contents_first + i} 0 R {extra_page_keys(i)}>>"
        )
    # Renumber: pages start at 3, so contents must follow them.
    objects = objects[:2] + [
        o.replace(f"/Contents {contents_first + i} 0 R",
                  f"/Contents {3 + len(pages) + i} 0 R")
         .replace(f"/Font << /F1 {contents_first + len(pages)} 0 R >>",
                  f"/Font << /F1 {3 + 2 * len(pages)} 0 R >>")
        for i, o in enumerate(objects[2:])
    ]
    for page in pages:
        objects.append(stream(page))
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    objects.extend(extra_objects)
    open(path, "wb").write(build(objects))
    return path


def make_test():
    """Three plain pages.

    "haystack" is on every page and "needle" only on page 1. z10 redacts the
    haystack line from page 1 and then asserts pages 2 and 3 still have theirs,
    which is how it proves the rewrite touched one page and not the document.
    The second line of each page has to fall inside QRectF(60, 650, 460, 25),
    which is where start_y and leading come from — measured with
    `pdftotext -bbox`, not guessed.
    """
    pages = [
        text_page(["MERGEN test page 1", "searchable haystack needle page 1",
                   "the quick brown fox"]),
        text_page(["MERGEN test page 2", "searchable haystack page 2",
                   "jumps over the lazy dog"]),
        text_page(["MERGEN test page 3", "searchable haystack page 3",
                   "pack my box with five dozen liquor jugs"]),
    ]
    simple("test.pdf", pages)


def make_outline(path="outline.pdf", second_page_text="second page body text"):
    """Nested table of contents, and an internal link on page 1 to page 3."""
    pages = [
        text_page(["MERGEN outline test page 1",
                   "searchable haystack needle page 1", "jump to page three"]),
        text_page(["MERGEN outline test page 2", second_page_text]),
        text_page(["MERGEN outline test page 3", "third page body text"]),
    ]
    # 1 catalog, 2 pages, 3-5 page objects, 6-8 contents, 9 font,
    # 10 outlines, 11-13 outline items, 14 link annotation.
    objects = [
        "<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R /PageMode /UseOutlines >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox %s /Resources << /Font << /F1 9 0 R >> >> "
        "/Contents 6 0 R /Annots [14 0 R] >>" % A4,
        "<< /Type /Page /Parent 2 0 R /MediaBox %s /Resources << /Font << /F1 9 0 R >> >> "
        "/Contents 7 0 R >>" % A4,
        "<< /Type /Page /Parent 2 0 R /MediaBox %s /Resources << /Font << /F1 9 0 R >> >> "
        "/Contents 8 0 R >>" % A4,
        stream(pages[0]), stream(pages[1]), stream(pages[2]),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        # Chapter One has no children; Section 2.1 hangs under Chapter Two. That
        # ordering is what makes a depth-first flatten come out as
        # [Chapter One d0 p0, Chapter Two d0 p1, Section 2.1 d1 p2].
        "<< /Type /Outlines /First 11 0 R /Last 12 0 R /Count 3 >>",
        "<< /Title (Chapter One) /Parent 10 0 R /Next 12 0 R /Dest [3 0 R /Fit] >>",
        "<< /Title (Chapter Two) /Parent 10 0 R /Prev 11 0 R /First 13 0 R /Last 13 0 R "
        "/Count 1 /Dest [4 0 R /Fit] >>",
        "<< /Title (Section 2.1) /Parent 12 0 R /Dest [5 0 R /Fit] >>",
        "<< /Type /Annot /Subtype /Link /Rect [60 660 260 700] /Border [0 0 0] "
        "/Dest [5 0 R /Fit] >>",
    ]
    open(path, "wb").write(build(objects))


def make_annot():
    """Highlight, underline and square annotations on page 1."""
    page = text_page(["MERGEN annotation test", "highlighted text here",
                      "underlined text here"])
    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox %s /Resources << /Font << /F1 5 0 R >> >> "
        "/Contents 4 0 R /Annots [6 0 R 7 0 R 8 0 R] >>" % A4,
        stream(page),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Type /Annot /Subtype /Highlight /Rect [55 700 320 740] "
        "/QuadPoints [55 740 320 740 55 700 320 700] /C [1 1 0] /CA 1 /F 4 >>",
        "<< /Type /Annot /Subtype /Underline /Rect [55 660 300 700] "
        "/QuadPoints [55 700 300 700 55 660 300 660] /C [0 0 1] /F 4 >>",
        "<< /Type /Annot /Subtype /Square /Rect [60 400 400 600] /C [1 0 0] "
        "/IC [0.9 0.9 1] /BS << /W 3 >> /F 4 >>",
    ]
    open("annot.pdf", "wb").write(build(objects))


def make_colour():
    """Flat swatches, so night mode can be checked for hue preservation."""
    swatches = []
    colours = [(1, 0, 0), (0, 0.6, 0), (0, 0, 1), (1, 1, 0), (0, 0.8, 0.8), (0.5, 0, 0.5)]
    for i, (r, g, b) in enumerate(colours):
        y = 700 - i * 110
        swatches.append(f"{r} {g} {b} rg 60 {y} 460 90 re f")
    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox %s /Resources << >> /Contents 4 0 R >>" % A4,
        stream("\n".join(swatches)),
    ]
    open("colour.pdf", "wb").write(build(objects))


def make_evil():
    """Embedded JavaScript and a form field — what the properties dialog warns about."""
    page = text_page(["MERGEN hostile document", "this file carries JavaScript",
                      "and an AcroForm field"])
    objects = [
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [7 0 R] >> "
        "/Names << /JavaScript << /Names [(evil) 6 0 R] >> >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox %s /Resources << /Font << /F1 5 0 R >> >> "
        "/Contents 4 0 R /Annots [7 0 R] >>" % A4,
        stream(page),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /S /JavaScript /JS (app.alert\\('this should never run'\\);) >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /Rect [60 300 300 330] "
        "/V (evil@attacker.example) /F 4 >>",
    ]
    open("evil.pdf", "wb").write(build(objects))


def make_huge():
    """One page at the PDF maximum, 14400x14400 pt. For the fit-mode floor."""
    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 14400 14400] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        stream("BT /F1 400 Tf 500 13000 Td (HUGE) Tj ET"),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    open("huge.pdf", "wb").write(build(objects))


def make_large(count=1000):
    """A thousand A4 pages, each with a searchable number. For cache bounds."""
    objects = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [%s] /Count %d >>"
        % (" ".join(f"{3 + i} 0 R" for i in range(count)), count),
    ]
    font_number = 3 + 2 * count
    for i in range(count):
        objects.append(
            f"<< /Type /Page /Parent 2 0 R /MediaBox {A4} "
            f"/Resources << /Font << /F1 {font_number} 0 R >> >> "
            f"/Contents {3 + count + i} 0 R >>"
        )
    for i in range(count):
        objects.append(stream(text_page([f"page {i + 1} of {count}",
                                         "searchable haystack needle" if i == 0 else "body"])))
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    open("large.pdf", "wb").write(build(objects))


def make_locked():
    """Password-protected, via qpdf. Skipped when qpdf is not installed."""
    if not shutil.which("qpdf"):
        print("  locked.pdf   SKIPPED — qpdf is not on the path")
        return False
    subprocess.run(
        ["qpdf", "--encrypt", "opensesame", "opensesame", "256", "--", "test.pdf",
         "locked.pdf"],
        check=True,
    )
    return True


def main():
    if len(sys.argv) > 1:
        os.chdir(sys.argv[1])
    print(f"writing fixtures into {os.getcwd()}")

    make_test()
    make_outline()
    # outline-v2 differs from outline on page 2 and nowhere else, which is what
    # compare mode is measured against.
    make_outline("outline-v2.pdf", second_page_text="second page CHANGED text")
    make_annot()
    make_colour()
    make_evil()
    make_huge()
    make_large()
    locked = make_locked()

    for name in ["test.pdf", "outline.pdf", "outline-v2.pdf", "annot.pdf", "colour.pdf",
                 "evil.pdf", "huge.pdf", "large.pdf"] + (["locked.pdf"] if locked else []):
        print(f"  {name:16} {os.path.getsize(name):>9,} bytes")

    # z9 copies outline.pdf under a second name to prove the content hash
    # follows the bytes rather than the path; z10 wrote its output here.
    print("\nNot written here, because the suites make them themselves:")
    print("  renamed.pdf, out.pdf, redacted.pdf, src.pdf, and the missing-file names")


if __name__ == "__main__":
    main()
