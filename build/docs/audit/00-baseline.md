# Baseline: my own instrumentation pass

Run before the agents reported, so their claims can be checked against
independent evidence rather than taken on assertion.

## ASan + UBSan — all ten acceptance suites

| suite | result | sanitizer hits |
|---|---|---|
| z1check | pass | 0 |
| z2check | pass | 0 |
| z3check | pass | 0 |
| z4check | pass | 0 |
| z5check | pass | 0 |
| z6check | pass | 0 |
| z7check | pass | 0 |
| z9check | pass | 0 |
| z10check | ABORTED | 1 — heap-use-after-free |

### CONFIRMED DEFECT — src/redact.cpp:224, heap-use-after-free

```cpp
QPDFWriter writer(pdf, destination.toLocal8Bit().constData());
//                     ^ temporary QByteArray, freed at the ";"
writer.write();   // :227 — qpdf dereferences the freed pointer here
```

QPDFWriter STORES the char* and uses it during write(). Line 202 uses the
same idiom safely, because processFile() consumes it synchronously.
Severity: high. Usually benign (freed bytes still hold the name), which is
exactly why ten passing acceptance runs never caught it.

## cppcheck

Clean apart from one nit (mergen-open.cpp:56 argv could be const) and one
config limitation (Q_SLOTS macro unknown to cppcheck). No defects.

## clazy (Qt-aware)

No correctness defects. Worth recording:

- redact.cpp:49 — Cut is a copyable polymorphic class (slicing risk).
  Safe as used via make_shared, but poor hygiene in this file of all files.
- range-loop-detach x5, detaching-member x3 — incl. cachedPage()'s
  QHash::find() on the paint path. Likely benign: a container with
  refcount 1 does not detach. VERIFY rather than assume.
- GlyphButton / Panel missing Q_OBJECT — deliberate, they need no
  metaobject. Document as intentional.
- ~12 function-args-by-value nits (QPoint/QSizeF passed by const ref).
