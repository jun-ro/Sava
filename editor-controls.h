#pragma once

#include <QPlainTextEdit>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include <functional>

class QToolButton;

namespace sava {

void fadeIn(QWidget *page);

class ModeSelector final : public QWidget
{
public:
    explicit ModeSelector(QWidget *parent = nullptr);
    bool previewing() const { return previewMode; }
    void setPreview(bool on);
    std::function<void(bool)> changed;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QToolButton *edit;
    QToolButton *preview;
    QVariantAnimation slide;
    bool previewMode = false;
    QRectF thumb{2, 3, 26, 24};
};

class WritingArea final : public QPlainTextEdit
{
public:
    explicit WritingArea(QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;

private:
    void toggleInline(const QString &marker);
    void insertLink();
    void trackCaret(bool animate);
    void stopCaret();
    QVariantAnimation glide;
    QTimer blink;
    QRectF paintedCaret;
    QRect target;
    bool caretOn = true;
    bool composing = false;
};

} // namespace sava
