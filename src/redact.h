// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QRectF>
#include <QString>

namespace mergen {

/// Removes text from a PDF, rather than drawing over it.
///
/// A black rectangle laid on top of a name is not redaction: the text sits
/// underneath, still selectable, still extractable, and every tool that ships
/// it that way is handing the reader a false guarantee. This walks the page's
/// content stream, drops the text-showing operators positioned inside the
/// area, and writes a new file — MZ.md §9.
///
/// Nothing is written in place. The source is never the destination.
class Redact {
public:
    struct Result {
        bool ok = false;
        /// A sentence for the reader. Empty on success.
        QString error;
    };

    /// Removes the text inside \a area on \a page of \a source and writes the
    /// result to \a destination.
    ///
    /// \a area is in PDF user space — points, origin at the bottom-left of the
    /// page — not in the top-left space the view works in.
    ///
    /// \a expected is the text the reader selected. It is what the result is
    /// checked against: the written file is reopened and searched, and if the
    /// text is still findable the file is refused rather than handed over as
    /// redacted. A redaction tool that cannot show the text is gone should say
    /// so, not imply it.
    static Result run(const QString &source, const QString &destination, int page,
                      const QRectF &area, const QString &expected);
};

} // namespace mergen
