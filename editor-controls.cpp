#include "editor-controls.h"

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QInputMethodEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QPropertyAnimation>
#include <QShortcut>
#include <QTextBlock>
#include <QToolButton>
#include <utility>

namespace sava {
namespace {

QIcon modeIcon(bool preview)
{
    QPixmap shape(32, 32);
    shape.setDevicePixelRatio(2);
    shape.fill(Qt::transparent);
    QPainter paint(&shape);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.setPen(QPen(QColor("#f3e9d5"), 1.2));
    if (preview) {
        paint.drawEllipse(QRectF(1.5, 4.5, 13, 7));
        paint.setBrush(QColor("#f3e9d5"));
        paint.drawEllipse(QRectF(6.55, 6.55, 2.9, 2.9));
    } else {
        paint.drawRoundedRect(QRectF(3.5, 1.5, 9, 13), 1.5, 1.5);
        paint.drawLine(QPointF(6, 6), QPointF(10, 6));
        paint.drawLine(QPointF(6, 9), QPointF(10, 9));
    }
    return QIcon(shape);
}

bool inlinePair(QStringView text, const QString &marker)
{
    const auto width = marker.size();
    if (text.size() < 2 * width || !text.startsWith(marker) || !text.endsWith(marker))
        return false;
    if (marker != QStringLiteral("*"))
        return true;

    // An even run of asterisks denotes bold. An odd run also includes italic.
    qsizetype left = 0;
    qsizetype right = 0;
    while (left < text.size() && text.at(left) == '*') ++left;
    while (right < text.size() && text.at(text.size() - right - 1) == '*') ++right;
    return left == text.size() ? text.size() == 2 : left % 2 != 0 && right % 2 != 0;
}

bool surroundedBy(QStringView line, qsizetype left, qsizetype right, const QString &marker)
{
    const auto width = marker.size();
    if (left < width || right + width > line.size()
        || line.mid(left - width, width) != marker || line.mid(right, width) != marker)
        return false;
    if (marker != QStringLiteral("*"))
        return true;
    auto begin = left - 1;
    auto end = right + 1;
    while (begin > 0 && line.at(begin - 1) == '*') --begin;
    while (end < line.size() && line.at(end) == '*') ++end;
    return (left - begin) % 2 != 0 && (end - right) % 2 != 0;
}

} // namespace

void fadeIn(QWidget *page)
{
    auto *effect = new QGraphicsOpacityEffect(page);
    effect->setOpacity(.38);
    page->setGraphicsEffect(effect);
    auto *motion = new QPropertyAnimation(effect, "opacity", effect);
    motion->setStartValue(.38);
    motion->setEndValue(1.0);
    motion->setDuration(185);
    motion->setEasingCurve(QEasingCurve::OutCubic);
    const QPointer<QGraphicsOpacityEffect> pending(effect);
    QObject::connect(motion, &QPropertyAnimation::finished, page, [page,pending] {
        QTimer::singleShot(0, page, [page,pending] {
            if (pending && page->graphicsEffect() == pending) page->setGraphicsEffect(nullptr);
        });
    });
    motion->start();
}

ModeSelector::ModeSelector(QWidget *parent) : QWidget(parent), edit(new QToolButton(this)), preview(new QToolButton(this))
{
    setFixedSize(58, 30);
    setStyleSheet("sava--ModeSelector { background: transparent; }"
                  "QToolButton { background: transparent; padding: 0; border-radius: 10px; }"
                  "QToolButton:enabled:hover { background: rgba(255,255,255,30); }"
                  "QToolButton:checked:enabled:hover { background: rgba(165,231,252,43); }");
    edit->setGeometry(2, 3, 26, 24);
    preview->setGeometry(30, 3, 26, 24);
    edit->setIcon(modeIcon(false));
    preview->setIcon(modeIcon(true));
    edit->setAccessibleName("Edit note");
    preview->setAccessibleName("Preview note");
    edit->setToolTip("Edit");
    preview->setToolTip("Preview");
    edit->setCheckable(true);
    preview->setCheckable(true);
    edit->setChecked(true);

    auto select = [this](bool on) {
        setPreview(on);
        if (changed) changed(on);
    };
    connect(edit, &QToolButton::clicked, this, [select] { select(false); });
    connect(preview, &QToolButton::clicked, this, [select] { select(true); });
    connect(&slide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        thumb = value.toRectF();
        update();
    });
}

void ModeSelector::setPreview(bool on)
{
    edit->setChecked(!on);
    preview->setChecked(on);
    if (on == previewMode) return;
    previewMode = on;
    const qreal center = on ? 43 : 15;
    slide.stop();
    slide.setStartValue(thumb);
    slide.setKeyValueAt(.47, QRectF((thumb.center().x() + center) / 2 - 17, 5, 34, 20));
    slide.setEndValue(QRectF(center - 13, 3, 26, 24));
    slide.setDuration(270);
    slide.setEasingCurve(QEasingCurve::InOutCubic);
    slide.start();
}

void ModeSelector::paintEvent(QPaintEvent *)
{
    QPainter paint(this);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.setPen(QColor("#3f3e3a"));
    paint.setBrush(Qt::NoBrush);
    paint.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5), 14, 14);
    paint.setPen(Qt::NoPen);
    paint.setBrush(QColor("#29424b"));
    paint.drawEllipse(thumb);
}

WritingArea::WritingArea(QWidget *parent) : QPlainTextEdit(parent)
{
    auto shortcut = [this](const QKeySequence &key, auto action) {
        auto *binding = new QShortcut(key, this);
        binding->setContext(Qt::WidgetShortcut);
        binding->setAutoRepeat(false);
        connect(binding, &QShortcut::activated, this, [this, action] {
            if (!isReadOnly() && !composing)
                action();
        });
    };
    shortcut(QKeySequence::Bold, [this] { toggleInline(QStringLiteral("**")); });
    shortcut(QKeySequence::Italic, [this] { toggleInline(QStringLiteral("*")); });
    shortcut(QKeySequence("Ctrl+Shift+X"), [this] { toggleInline(QStringLiteral("~~")); });
    shortcut(QKeySequence("Ctrl+K"), [this] { insertLink(); });

    glide.setDuration(85);
    glide.setEasingCurve(QEasingCurve::OutCubic);
    connect(&glide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        const auto old = paintedCaret;
        paintedCaret = value.toRectF();
        viewport()->update(old.united(paintedCaret).toAlignedRect().adjusted(-2,-2,2,2));
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] { trackCaret(true); });
    connect(this, &QPlainTextEdit::selectionChanged, this, [this] { trackCaret(false); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &, int dy) {
        if (dy) trackCaret(false);
    });
    connect(&blink, &QTimer::timeout, this, [this] {
        caretOn = !caretOn;
        viewport()->update(paintedCaret.toAlignedRect().adjusted(-2,-2,2,2));
    });
}

void WritingArea::toggleInline(const QString &marker)
{
    const auto original = textCursor();
    const int start = original.selectionStart();
    const int end = original.selectionEnd();
    const int width = marker.size();
    const bool selected = original.hasSelection();
    QTextCursor edit(document());
    edit.beginEditBlock();

    int selectionStart = start;
    int selectionEnd = end;
    // Edit from the last block back so changes leave earlier offsets intact.
    for (auto block = document()->findBlock(selected ? end - 1 : end);
         block.isValid() && block.position() + block.length() > start;
         block = block.previous()) {
        const QString line = block.text();
        const int offset = block.position();
        const int from = qMax(0, start - offset);
        const int to = qMin(int(line.size()), end - offset);
        int left = from;
        int right = to;
        while (left < right && line.at(left).isSpace()) ++left;
        while (right > left && line.at(right - 1).isSpace()) --right;
        if (selected && left == right) continue;

        const auto text = QStringView(line).mid(left, right - left);
        QString replacement;
        int contentStart = width;
        int contentLength = text.size();
        int replaceFrom = left;
        int replaceTo = right;
        if (inlinePair(text, marker)) {
            replacement = text.sliced(width, text.size() - 2 * width).toString();
            contentStart = 0;
            contentLength = replacement.size();
        } else if (surroundedBy(QStringView(line), left, right, marker)) {
            replacement = text.toString();
            replaceFrom -= width;
            replaceTo += width;
            contentStart = 0;
        } else {
            replacement = marker + text + marker;
        }

        edit.setPosition(offset + replaceFrom);
        edit.setPosition(offset + replaceTo, QTextCursor::KeepAnchor);
        edit.insertText(replacement);
        const int difference = replacement.size() - (replaceTo - replaceFrom);
        const int newBegin = offset + replaceFrom + contentStart;
        if (from == start - offset) selectionStart = newBegin;
        if (to == end - offset)
            selectionEnd = newBegin + contentLength;
        else
            selectionEnd += difference;
    }

    edit.endEditBlock();
    if (selected && original.anchor() > original.position())
        std::swap(selectionStart, selectionEnd);
    edit.setPosition(selectionStart);
    edit.setPosition(selectionEnd, QTextCursor::KeepAnchor);
    setTextCursor(edit);
}

void WritingArea::insertLink()
{
    auto cursor = textCursor();
    const bool selected = cursor.hasSelection();
    QString label = cursor.selectedText();
    if (label.contains(QChar::ParagraphSeparator) || label.contains(QLatin1Char('\n')))
        return;
    label.replace('\\', QStringLiteral("\\\\"));
    label.replace('[', QStringLiteral("\\["));
    label.replace(']', QStringLiteral("\\]"));
    if (label.isEmpty()) label = QStringLiteral("link text");

    const QString url = QStringLiteral("https://");
    const int start = cursor.selectionStart();
    cursor.insertText('[' + label + QStringLiteral("](") + url + ')');
    const int selection = selected ? start + label.size() + 3 : start + 1;
    cursor.setPosition(selection);
    cursor.setPosition(selection + (selected ? url.size() : label.size()), QTextCursor::KeepAnchor);
    setTextCursor(cursor);
}

void WritingArea::trackCaret(bool animate)
{
    if (isReadOnly() || !hasFocus() || textCursor().hasSelection() || composing ||
        QApplication::cursorFlashTime() <= 0) {
        stopCaret();
        return;
    }
    auto position = cursorRect();
    position.setWidth(2);
    if (position == target && animate) return;
    target = position;
    setCursorWidth(0);
    caretOn = true;
    blink.start(qMax(250, QApplication::cursorFlashTime() / 2));
    const auto old = paintedCaret;
    glide.stop();
    if (animate && !old.isEmpty() && qAbs(old.center().y() - target.center().y()) < 80) {
        glide.setStartValue(old);
        glide.setEndValue(QRectF(target));
        glide.start();
    } else {
        paintedCaret = target;
        viewport()->update(old.united(paintedCaret).toAlignedRect().adjusted(-2,-2,2,2));
    }
}

void WritingArea::stopCaret()
{
    glide.stop();
    blink.stop();
    const auto old = paintedCaret;
    target = {};
    paintedCaret = {};
    setCursorWidth(1);
    viewport()->update(old.toAlignedRect().adjusted(-2,-2,2,2));
}

void WritingArea::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);
    if (caretOn && !paintedCaret.isEmpty() && !isReadOnly()) {
        QPainter paint(viewport());
        paint.fillRect(paintedCaret, QColor("#70cdef"));
    }
}

void WritingArea::focusInEvent(QFocusEvent *event)
{
    QPlainTextEdit::focusInEvent(event);
    trackCaret(false);
}

void WritingArea::focusOutEvent(QFocusEvent *event)
{
    stopCaret();
    QPlainTextEdit::focusOutEvent(event);
}

void WritingArea::inputMethodEvent(QInputMethodEvent *event)
{
    composing = !event->preeditString().isEmpty();
    QPlainTextEdit::inputMethodEvent(event);
    trackCaret(false);
}

} // namespace sava
