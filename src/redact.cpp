// MERGEN — a minimal PDF viewer.
// Copyright (C) 2026 MEGAS.
// SPDX-License-Identifier: GPL-3.0-only

#include "redact.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QPointF>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFTokenizer.hh>
#include <qpdf/QPDFWriter.hh>

#include <poppler-qt6.h>

#include <array>
#include <memory>
#include <vector>

namespace mergen {
namespace {

/// A 2-D affine matrix in PDF order: a b c d e f.
struct Matrix {
    std::array<double, 6> m{1, 0, 0, 1, 0, 0};

    /// this × other, in PDF's convention where the left operand applies first.
    Matrix times(const Matrix &o) const {
        return Matrix{{m[0] * o.m[0] + m[1] * o.m[2], m[0] * o.m[1] + m[1] * o.m[3],
                       m[2] * o.m[0] + m[3] * o.m[2], m[2] * o.m[1] + m[3] * o.m[3],
                       m[4] * o.m[0] + m[5] * o.m[2] + o.m[4],
                       m[4] * o.m[1] + m[5] * o.m[3] + o.m[5]}};
    }

    /// Where the origin of this space lands.
    QPointF origin() const { return QPointF(m[4], m[5]); }
};

/// Drops the text-showing operators whose position falls inside one rectangle.
///
/// The state machine is the whole of it: a show-text operator carries no
/// position, only the text, so where it lands has to be tracked through the
/// graphics and text matrices that preceded it.
class Cut : public QPDFObjectHandle::TokenFilter {
public:
    Cut(const QRectF &area, int *removed) : m_area(area), m_removed(removed) {}

    void handleToken(QPDFTokenizer::Token const &token) override {
        // Operands accumulate until an operator arrives to consume them.
        if (token.getType() != QPDFTokenizer::tt_word) {
            if (token.getType() != QPDFTokenizer::tt_space &&
                token.getType() != QPDFTokenizer::tt_comment) {
                m_operands.push_back(token);
            }
            m_pending.push_back(token);
            return;
        }

        const std::string op = token.getValue();
        m_pending.push_back(token);

        if (op == "q") {
            m_stack.push_back(m_ctm);
        } else if (op == "Q") {
            if (!m_stack.empty()) {
                m_ctm = m_stack.back();
                m_stack.pop_back();
            }
        } else if (op == "cm") {
            m_ctm = numbers(6).times(m_ctm);
        } else if (op == "BT") {
            m_text = Matrix();
            m_line = Matrix();
        } else if (op == "Tm") {
            m_text = numbers(6);
            m_line = m_text;
        } else if (op == "TL") {
            m_leading = operand(1, 0);
        } else if (op == "Td" || op == "TD") {
            if (op == "TD") {
                m_leading = -operand(2, 1);
            }
            m_line = Matrix{{1, 0, 0, 1, operand(2, 0), operand(2, 1)}}.times(m_line);
            m_text = m_line;
        } else if (op == "T*") {
            m_line = Matrix{{1, 0, 0, 1, 0, -m_leading}}.times(m_line);
            m_text = m_line;
        } else if (op == "'" || op == "\"") {
            m_line = Matrix{{1, 0, 0, 1, 0, -m_leading}}.times(m_line);
            m_text = m_line;
        }

        const bool shows = op == "Tj" || op == "TJ" || op == "'" || op == "\"";
        if (shows && inside()) {
            // Drop the operator and everything it was given. What is not
            // written is not in the file.
            ++*m_removed;
            m_pending.clear();
            m_operands.clear();
            return;
        }

        flush();
        m_operands.clear();
    }

    void handleEOF() override { flush(); }

private:
    void flush() {
        for (const auto &t : m_pending) {
            writeToken(t);
        }
        m_pending.clear();
    }

    /// Operand \a index of the \a total the operator takes. Operands are read
    /// back from the operator, since a content stream writes them before it.
    double operand(size_t total, size_t index) const {
        if (m_operands.size() < total || index >= total) {
            return 0.0;
        }
        try {
            return std::stod(m_operands[m_operands.size() - total + index].getValue());
        } catch (...) {
            return 0.0;
        }
    }

    Matrix numbers(size_t count) const {
        Matrix out;
        if (m_operands.size() < count) {
            return out;
        }
        for (size_t i = 0; i < count; ++i) {
            try {
                out.m[i] = std::stod(m_operands[m_operands.size() - count + i].getValue());
            } catch (...) {
                out.m[i] = (i == 0 || i == 3) ? 1.0 : 0.0;
            }
        }
        return out;
    }

    bool inside() const {
        const QPointF at = m_text.times(m_ctm).origin();
        return m_area.contains(at);
    }

    QRectF m_area;
    int *m_removed;
    Matrix m_ctm;
    std::vector<Matrix> m_stack;
    Matrix m_text;
    Matrix m_line;
    double m_leading = 0.0;
    std::vector<QPDFTokenizer::Token> m_operands;
    std::vector<QPDFTokenizer::Token> m_pending;
};

/// Is the text still findable on that page of the written file?
bool stillThere(const QString &path, int page, const QString &needle) {
    if (needle.trimmed().isEmpty()) {
        return false;
    }
    const std::unique_ptr<Poppler::Document> doc = Poppler::Document::load(path);
    if (!doc || doc->isLocked()) {
        // Unreadable output cannot be shown to be clean, so it is not claimed.
        return true;
    }
    const std::unique_ptr<Poppler::Page> p = doc->page(page);
    if (!p) {
        return true;
    }
    const QString text = p->text(QRectF()).simplified();
    // Compare on the longest run the selection gave, since a word may be split
    // across show operators and rejoined differently by extraction.
    for (const QString &word : needle.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (word.size() >= 3 && text.contains(word)) {
            return true;
        }
    }
    return false;
}

} // namespace

Redact::Result Redact::run(const QString &source, const QString &destination, int page,
                           const QRectF &area, const QString &expected) {
    if (QFileInfo(source).absoluteFilePath() == QFileInfo(destination).absoluteFilePath()) {
        return {false, QObject::tr("Redaction never writes over the document it read.")};
    }

    int removed = 0;
    try {
        QPDF pdf;
        pdf.processFile(source.toLocal8Bit().constData());

        QPDFPageDocumentHelper pages(pdf);
        std::vector<QPDFPageObjectHelper> all = pages.getAllPages();
        if (page < 0 || static_cast<size_t>(page) >= all.size()) {
            return {false, QObject::tr("That page is not in the document.")};
        }

        QPDFPageObjectHelper target = all[static_cast<size_t>(page)];

        // addContentTokenFilter, not filterContents. filterContents runs the
        // filter through a pipeline and leaves the page untouched; only this
        // rewrites the content stream when the document is written.
        target.addContentTokenFilter(std::make_shared<Cut>(area, &removed));

        // A mark over the gap, so the page shows that something was taken out
        // rather than quietly closing over it.
        const std::string cover = "q 0 0 0 rg " + std::to_string(area.x()) + " " +
                                  std::to_string(area.y()) + " " + std::to_string(area.width()) +
                                  " " + std::to_string(area.height()) + " re f Q\n";
        target.addPageContents(QPDFObjectHandle::newStream(&pdf, cover), false);

        QPDFWriter writer(pdf, destination.toLocal8Bit().constData());
        writer.setStaticID(false);
        // The filter runs here, so the count is only known afterwards.
        writer.write();

        if (removed == 0) {
            QFile::remove(destination);
            return {false, QObject::tr("Nothing removable was found in that area.")};
        }
    } catch (const std::exception &e) {
        return {false, QObject::tr("The document could not be rewritten: %1")
                           .arg(QString::fromUtf8(e.what()))};
    }

    if (stillThere(destination, page, expected)) {
        // Refuse rather than hand over a file that looks redacted and is not.
        QFile::remove(destination);
        return {false, QObject::tr("The text is still in the rewritten file, so it was discarded. "
                                   "Nothing was saved.")};
    }

    return {true, QString()};
}

} // namespace mergen
