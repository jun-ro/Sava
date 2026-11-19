#include "chat.h"
#include "discord-rpc.h"
#include "editor-controls.h"
#include "model-catalog.h"
#include "workspace.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSyntaxHighlighter>
#include <QTabBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

#ifdef Q_OS_WIN
void applyWindowsTitleBar(QWidget *window)
{
    if (!window)
        return;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;
    const BOOL dark = TRUE;
    constexpr DWORD kUseImmersiveDarkMode = 20;
    constexpr DWORD kUseImmersiveDarkModeLegacy = 19;
    if (FAILED(::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark))))
        ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkModeLegacy, &dark, sizeof(dark));
    constexpr DWORD kCaptionColor = 35;
    constexpr DWORD kTextColor = 36;
    const COLORREF caption = RGB(0x1d, 0x1d, 0x1d);
    const COLORREF text = RGB(0xf3, 0xe9, 0xd5);
    ::DwmSetWindowAttribute(hwnd, kCaptionColor, &caption, sizeof(caption));
    ::DwmSetWindowAttribute(hwnd, kTextColor, &text, sizeof(text));
}

constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
constexpr const char *kTrashName = "Recycle Bin";
#else
void applyWindowsTitleBar(QWidget *) {}
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
constexpr const char *kTrashName = "Trash";
#endif

bool samePath(const QString &a, const QString &b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), kPathCase) == 0;
}

bool pathUnderRoot(const QString &file, const QString &root)
{
    if (file.isEmpty() || root.isEmpty())
        return false;
    QString prefix = QDir::cleanPath(root);
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    return QDir::cleanPath(file).startsWith(prefix, kPathCase);
}

const auto style = R"(
QMainWindow, QWidget { background: #171717; color: #f3e9d5; }
QWidget#rail, QWidget#chrome, QTreeWidget, QLineEdit { background: #1d1d1d; }
QLabel { background: transparent; }
QLabel#wordmark { font-size: 24px; font-weight: 900; }
QLabel#path, QLabel#stats { color: #b9b3a9; }
QTabBar#noteTabs::tab { background: transparent; color: #b9b3a9; border-radius: 12px; margin-right: 4px; padding: 4px 8px; max-width: 174px; min-height: 18px; }
QTabBar#noteTabs::tab:hover { background: #242424; }
QTabBar#noteTabs::tab:selected { background: #292725; color: #f3e9d5; font-weight: 700; }
QTabBar#noteTabs::tab:selected:hover { background: #393632; }
QWidget#tabCloseSlot { background: transparent; }
QToolButton[tabClose="true"] { padding: 0; border-radius: 5px; }
QLabel#settingsTitle { font-size: 30px; font-weight: 900; }
QFrame[settingsGroup="true"] { background: #202020; border: 1px solid #393734; border-radius: 12px; }
QSpinBox#settingValue, QLineEdit#settingInput { background: #252525; color: #f3e9d5; border: 1px solid #41403d; border-radius: 7px; padding: 5px 8px; }
QSpinBox#settingValue:focus, QLineEdit#settingInput:focus { border-color: #70cdef; }
QLabel#settingsSection { font-weight: 700; color: #f3e9d5; }
QLabel#settingsHint { color: #9c978f; font-size: 11px; }
QCheckBox { background: transparent; spacing: 8px; }
QCheckBox::indicator { width: 15px; height: 15px; border-radius: 5px; border: 1px solid #484643; background: #252525; }
QCheckBox::indicator:hover { border-color: #70cdef; }
QCheckBox::indicator:checked { background: #76e3b4; border-color: #76e3b4; }
QPushButton#openAgentSettings { background: #263740; color: #a5e7fc; padding: 6px 12px; font-weight: 650; }
QPushButton#openAgentSettings:hover { background: #304750; }
QToolButton, QPushButton { background: transparent; border: 0; border-radius: 12px; padding: 3px 8px; }
QToolButton:enabled:hover, QPushButton:enabled:hover { background: #383532; }
QToolButton:checked { background: #263740; color: #70cdef; }
QToolButton:checked:enabled:hover { background: #344952; }
QToolButton:focus, QPushButton:focus, QLineEdit:focus { border: 1px solid #70cdef; }
QLineEdit { border: 1px solid #353535; border-radius: 13px; padding: 4px 9px; }
QTreeWidget { border: 0; padding: 1px; outline: 0; }
QTreeWidget::item { padding: 4px 2px; border-radius: 10px; }
QTreeWidget::item:selected { background: #24342d; color: #76e3b4; }
QTreeWidget::item:hover:!selected { background: #292826; }
QTreeView::branch:selected { background: #1d1d1d; }
QPlainTextEdit, QTextBrowser { border: none; background: #171717; selection-background-color: #3c5460; }
QSplitter::handle { background: #353535; }
QScrollBar:vertical { background: transparent; width: 8px; }
QScrollBar::handle:vertical { background: #42413e; min-height: 20px; border-radius: 3px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QProgressBar { background: #373532; border: 0; border-radius: 3px; }
QProgressBar::chunk { border-radius: 3px; background: #ff80a7; }
QProgressBar#context::chunk {
 background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ffda70, stop:.34 #ff80a7, stop:.69 #76e3b4, stop:1 #70cdef);
}

)";

class ButtonCursors final : public QObject
{
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Polish) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && (qobject_cast<QAbstractButton *>(widget) ||
                           qobject_cast<QComboBox *>(widget) ||
                           qobject_cast<QTabBar *>(widget) ||
                           qobject_cast<QMenu *>(widget)))
                widget->setCursor(Qt::PointingHandCursor);
        }
        return false;
    }
};

constexpr int FolderPathRole = Qt::UserRole + 1;

class FileTree final : public QTreeWidget
{
public:
    explicit FileTree(QWidget *parent) : QTreeWidget(parent)
    {
        setAccessibleName("File explorer");
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setExpandsOnDoubleClick(false);
        setAnimated(true);
        connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
            if (plainRowClick && rowClicked)
                rowClicked(item);
        });
    }

    std::function<void(QTreeWidgetItem *)> rowClicked;
    std::function<void(QTreeWidgetItem *, const QPoint &)> menuRequested;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            event->accept();
            return;
        }
        QTreeWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const QPoint point = event->position().toPoint();
        const auto index = indexAt(point);
        const bool activate = event->button() == Qt::LeftButton
            && event->modifiers() == Qt::NoModifier && visualRect(index).contains(point);
        const QScopedValueRollback guard(plainRowClick, activate);
        QTreeWidget::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (!menuRequested)
            return;
        if (event->reason() == QContextMenuEvent::Keyboard && currentItem()) {
            const QPoint center = visualItemRect(currentItem()).center();
            menuRequested(currentItem(), viewport()->mapToGlobal(center));
        } else {
            menuRequested(itemAt(event->pos()), event->globalPos());
        }
    }

private:
    bool plainRowClick = false;
};

QIcon tabDot()
{
    QPixmap dot(18,18);
    dot.setDevicePixelRatio(2);
    dot.fill(Qt::transparent);
    QPainter paint(&dot);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.setPen(Qt::NoPen);
    paint.setBrush(QColor("#76e3b4"));
    paint.drawEllipse(QRectF(1.5,1.5,6,6));
    return QIcon(dot);
}

QToolButton *tool(const QString &text, const QString &label, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(label);
    button->setAccessibleName(label);
    return button;
}

QToolButton *iconTool(const QIcon &icon, const QString &label, QWidget *parent)
{
    auto *button = tool({}, label, parent);
    button->setIcon(icon);
    button->setIconSize(QSize(20, 20));
    button->setFixedSize(34, 30);
    return button;
}

QIcon crossIcon(bool close)
{
    const int size = close ? 12 : 20;
    QPixmap glyph(size * 2, size * 2);
    glyph.setDevicePixelRatio(2);
    glyph.fill(Qt::transparent);
    QPainter painter(&glyph);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#f3e9d5"), close ? 1.5 : 2.2,
                        Qt::SolidLine, Qt::RoundCap));
    painter.translate(size / 2.0, size / 2.0);
    if (close)
        painter.rotate(45);
    const double arm = close ? 4 : 6;
    painter.drawLine(QPointF(-arm, 0), QPointF(arm, 0));
    painter.drawLine(QPointF(0, -arm), QPointF(0, arm));
    return QIcon(glyph);
}

QString quotaPercent(double percent)
{
    return QLocale().toString(percent, 'g', 4) + '%';
}

QString quotaDetails(const sava::QuotaWindow &window, const QLocale &locale)
{
    QString label = "Quota window";
    if (window.minutes > 0) {
        if (window.minutes % 1440 == 0)
            label = QString("%1d window").arg(window.minutes / 1440);
        else if (window.minutes % 60 == 0)
            label = QString("%1h window").arg(window.minutes / 60);
        else
            label = QString("%1 min window").arg(window.minutes);
    }
    QString detail = QString("%1 · %2 used").arg(label, quotaPercent(window.usedPercent));
    if (window.resetsAt.isValid())
        detail += QString(" · resets %1").arg(locale.toString(window.resetsAt.toLocalTime(), QLocale::ShortFormat));
    return detail;
}

QIcon treeIcon(bool folder, const QColor &color)
{
    QPixmap glyph(28, 28);
    glyph.setDevicePixelRatio(2);
    glyph.fill(Qt::transparent);
    QPainter paint(&glyph);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.setPen(QPen(color, 1));
    if (folder) {
        paint.drawRoundedRect(QRectF(1.5, 4.5, 11, 7.5), 1.5, 1.5);
        paint.drawLine(QPointF(2, 4.5), QPointF(2, 2.5));
        paint.drawLine(QPointF(2, 2.5), QPointF(6.5, 2.5));
    } else {
        paint.drawRoundedRect(QRectF(3, 1.5, 8, 11), 1, 1);
        paint.drawLine(QPointF(5, 6), QPointF(9, 6));
        paint.drawLine(QPointF(5, 8), QPointF(9, 8));
    }
    return QIcon(glyph);
}

QIcon attachFolderIcon()
{
    QPixmap glyph(40, 40);
    glyph.setDevicePixelRatio(2);
    glyph.fill(Qt::transparent);
    QPainter paint(&glyph);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.setPen(QPen(QColor("#f3e9d5"), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath outline;
    outline.moveTo(2, 15);
    outline.lineTo(2, 5);
    outline.quadTo(2, 3.5, 3.5, 3.5);
    outline.lineTo(7.5, 3.5);
    outline.lineTo(10, 6);
    outline.lineTo(16.5, 6);
    outline.quadTo(18, 6, 18, 7.5);
    outline.lineTo(18, 15);
    outline.quadTo(18, 16.5, 16.5, 16.5);
    outline.lineTo(3.5, 16.5);
    outline.quadTo(2, 16.5, 2, 15);
    outline.closeSubpath();
    paint.drawPath(outline);
    return QIcon(glyph);
}

QIcon sidebarIcon()
{
    QPixmap glyph(40, 40);
    glyph.setDevicePixelRatio(2);
    glyph.fill(Qt::transparent);
    QPainter paint(&glyph);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.setPen(QPen(QColor("#f3e9d5"), 1.7, Qt::SolidLine, Qt::RoundCap));
    paint.drawRoundedRect(QRectF(2, 3.5, 16, 13), 2, 2);
    paint.drawLine(QPointF(8, 4), QPointF(8, 16));
    paint.drawLine(QPointF(5, 7), QPointF(5, 13));
    return QIcon(glyph);
}

QIcon settingsIcon()
{
    QPixmap glyph(40, 40);
    glyph.setDevicePixelRatio(2);
    glyph.fill(Qt::transparent);
    QPainter paint(&glyph);
    paint.setRenderHint(QPainter::Antialiasing);
    paint.translate(10.0, 10.0);
    paint.setPen(QPen(QColor("#f3e9d5"), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    paint.drawEllipse(QPointF(0, 0), 3.0, 3.0);
    for (int i = 0; i < 8; ++i) {
        const qreal angle = i * 3.14159265358979323846 / 4.0;
        const qreal c = std::cos(angle);
        const qreal s = std::sin(angle);
        paint.drawLine(QPointF(c * 5.2, s * 5.2), QPointF(c * 7.2, s * 7.2));
    }
    paint.drawEllipse(QPointF(0, 0), 5.4, 5.4);
    return QIcon(glyph);
}

class NoteStyle final : public QSyntaxHighlighter
{
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

protected:
    void highlightBlock(const QString &line) override
    {
        QTextCharFormat marker;
        marker.setForeground(Qt::transparent);
        marker.setFontPointSize(1);
        if (line.startsWith("# ")) {
            QTextCharFormat heading;
            heading.setFontWeight(QFont::Black);
            heading.setFontPointSize(28);
            heading.setForeground(QColor("#f3e9d5"));
            setFormat(0, line.size(), heading);
            setFormat(0, 2, marker);
        } else if (line.startsWith("## ") || line.startsWith("### ")) {
            QTextCharFormat heading;
            heading.setFontWeight(QFont::Bold);
            heading.setFontPointSize(16);
            setFormat(0, line.size(), heading);
            setFormat(0, line.startsWith("### ") ? 4 : 3, marker);
        }
        static const QRegularExpression link(R"(\[\[[^\]]+\]\])");
        static const QRegularExpression emphasis(R"((?<!\*)(\*{1,3})([^*]+?)\1(?!\*))");
        static const QRegularExpression strike(R"(~~([^~]+)~~)");
        QTextCharFormat accent;
        accent.setForeground(QColor("#70cdef"));
        for (const auto &match : link.globalMatch(line))
            setFormat(match.capturedStart(), match.capturedLength(), accent);
        for (const auto &match : emphasis.globalMatch(line)) {
            const auto width = match.capturedLength(1);
            auto formatted = format(match.capturedStart(2));
            if (width >= 2) formatted.setFontWeight(QFont::Bold);
            if (width % 2 != 0) formatted.setFontItalic(true);
            setFormat(match.capturedStart(2), match.capturedLength(2), formatted);
        }
        for (const auto &match : strike.globalMatch(line)) {
            auto formatted = format(match.capturedStart(1));
            formatted.setFontStrikeOut(true);
            setFormat(match.capturedStart(1), match.capturedLength(1), formatted);
        }
    }
};

class StarButton final : public QToolButton
{
public:
    explicit StarButton(QWidget *parent) : QToolButton(parent)
    {
        setCheckable(true);
        setFixedSize(34, 32);
        setToolTip("Co-write");
        setAccessibleName("Co-write");
        motion.setDuration(660);
        phase.start();
        motion.setStartValue(0.0);
        motion.setEndValue(1.0);
        connect(&motion, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) { hoverProgress = value.toDouble(); update(); });
        grain = QPixmap(size());
        grain.fill(Qt::transparent);
        QPainter speckle(&grain);
        quint32 seed = 39;
        for (int y = 0; y < grain.height(); ++y) {
            for (int x = 0; x < grain.width(); ++x) {
                seed = 1664525U * seed + 1013904223U;
                const int amount = int(seed >> 28) - 8;
                speckle.setPen(amount > 0 ? QColor(255,255,255,amount * 2)
                                         : QColor(0,0,0,-amount * 2));
                speckle.drawPoint(x,y);
            }
        }
        idle.setInterval(90);
        connect(&idle, &QTimer::timeout, this, [this] {
            if (!window()->isMinimized()) update();
        });
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QToolButton::showEvent(event);
        phase.restart();
        idle.start();
    }

    void hideEvent(QHideEvent *event) override
    {
        idle.stop();
        motion.stop();
        hoverProgress = 0;
        QToolButton::hideEvent(event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        if (isEnabled() && motion.state() == QAbstractAnimation::Stopped)
            motion.start();
        QToolButton::enterEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(rect().adjusted(1, 1, -1, -1), 14, 14);
        painter.setClipPath(clip);
        const bool hovered = isEnabled() && underMouse();
        painter.fillPath(clip, hovered ? QColor("#34312e")
                                       : isChecked() ? QColor("#20201e") : QColor("#131313"));
        constexpr qreal pi = 3.141592653589793;
        const qreal lift = std::sin(pi * hoverProgress);
        const qreal swing = lift * std::sin(2 * pi * hoverProgress);
        const qreal idleBend = .45 * std::sin(phase.elapsed() / 1300.0);
        const QColor colors[] = {"#ffda70", "#ff80a7", "#76e3b4", "#70cdef"};
        for (int stripe = 0; stripe < 4; ++stripe) {
            const qreal y = 21 + 2.7 * stripe;
            const qreal wavePhase = 9 * hoverProgress - .58 * stripe;
            const qreal crest = idleBend + 3.4 * lift * std::sin(wavePhase);
            const qreal tail = idleBend + 3.4 * lift * std::sin(wavePhase - 1.3);
            QPainterPath wave(QPointF(-1, y));
            wave.cubicTo(10, y - 2, 14, y + 4 + crest, 21, y - 4 + crest);
            wave.cubicTo(25, y - 8 + tail, 26, y - 11 + tail, 35, y - 11);
            painter.setPen(QPen(colors[stripe], 1.6));
            painter.drawPath(wave);
        }
        painter.drawPixmap(0, 0, grain);

        painter.save();
        painter.translate(17 + 1.0 * swing, 15 - 2.8 * lift);
        painter.rotate(24 * swing - 6 * lift);
        painter.scale(1 + .19 * lift, 1 + .19 * lift);
        const QPolygonF star{{0,-8},{2.4,-2.5},{8,0},{2.4,2.5},{0,8},{-2.4,2.5},{-8,0},{-2.4,-2.5}};
        painter.setPen(QPen(QColor("#141414"), 1.2));
        painter.setBrush(Qt::white);
        painter.drawPolygon(star);
        painter.restore();

        if (lift > .02) {
            painter.setPen(Qt::NoPen);
            painter.setOpacity(.72 * lift);
            painter.setBrush(QColor("#f3e9d5"));
            const qreal x = 26 + 2 * lift;
            const qreal y = 9 - 2 * lift;
            painter.drawPolygon(QPolygonF{{x, y-2.2}, {x+1.5, y}, {x, y+2.2}, {x-1.5, y}});
            painter.setBrush(colors[3]);
            painter.drawEllipse(QPointF(8 - lift, 12 - 2 * lift), .85, .85);
            painter.setOpacity(1);
        }

        const bool keyboardFocus = hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange);
        if (isChecked() || keyboardFocus || hovered) {
            painter.setClipping(false);
            painter.setPen(QPen(keyboardFocus ? QColor(171, 211, 222, 168)
                                              : QColor(243, 233, 213, 72)));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(clip);
        }
    }

private:
    QVariantAnimation motion;
    QPixmap grain;
    QTimer idle;
    QElapsedTimer phase;
    qreal hoverProgress = 0;
};

class Window final : public QMainWindow
{
public:
    explicit Window(const QString &initial)
    {
        resize(1040, 710);
        setMinimumSize(560, 380);
        setWindowTitle("Sava");
        auto *base = new QWidget(this);
        auto *body = new QVBoxLayout(base);
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
        setCentralWidget(base);

        auto *chrome = new QWidget(base);
        chrome->setObjectName("chrome");
        chrome->setFixedHeight(51);
        auto *top = new QHBoxLayout(chrome);
        top->setContentsMargins(13, 4, 10, 4);
        auto *brand = new QLabel("Sava.", chrome);
        brand->setObjectName("wordmark");
        top->addWidget(brand);
        top->addStretch();
        showFiles = iconTool(sidebarIcon(), "Show sidebar", chrome);
        top->addWidget(showFiles);
        showFiles->hide();
        settingsButton = iconTool(settingsIcon(), "Settings", chrome);
        settingsButton->setToolTip("Settings (Ctrl+,)");
        settingsButton->setCheckable(true);
        top->addWidget(settingsButton);
        star = new StarButton(chrome);
        top->addWidget(star);
        body->addWidget(chrome);

        pages = new QStackedWidget(base);
        body->addWidget(pages, 1);
        panes = new QSplitter(pages);
        panes->setChildrenCollapsible(false);
        panes->setHandleWidth(1);
        pages->addWidget(panes);
        makeFiles();
        makeEditor();
        const int leftWidth = sidebarWidth();
        panes->setSizes({leftWidth, width() - leftWidth});
        connect(showFiles, &QToolButton::clicked, this, [this] {
            showFiles->hide();
            slidePane(files, true, sidebarWidth(), fileSlide);
            search->setFocus();
        });
        connect(star, &QToolButton::toggled, this, [this](bool open) {
            showAppSettings(false);
            setChatVisible(open);
        });
        connect(settingsButton, &QToolButton::clicked, this, [this](bool open) {
            showAppSettings(open);
        });
        connect(new QShortcut(QKeySequence("Ctrl+,"), this), &QShortcut::activated, this,
                [this] { showAppSettings(pages->currentWidget() == panes); });

        words = new QLabel(this);
        words->setObjectName("stats");
        statusBar()->addWidget(words);
        statusBar()->addPermanentWidget(new QLabel("Markdown   UTF-8", this));
        autosave.setSingleShot(true);
        autosave.setInterval(500);
        connect(&autosave, &QTimer::timeout, this, [this] {
            saveCurrent();
            updateWords();
            syncChat();
        });
        connect(source, &QPlainTextEdit::textChanged, this, [this] {
            if (loading) return;
            if (!current.isEmpty()) autosave.start();
        });
        connect(new QShortcut(QKeySequence::Save, this), &QShortcut::activated, this, [this] { saveCurrent(); });
        connect(new QShortcut(QKeySequence::New, this), &QShortcut::activated, this, [this] { create(); });
        connect(new QShortcut(QKeySequence::Open, this), &QShortcut::activated, this, [this] { attach(); });

        QSettings settings;
        applyTextSize(qBound(10, settings.value("editorTextSize", 12).toInt(), 24));
        discordEnabled = settings.value("discordPresenceEnabled", false).toBool();
        discordShowTitle = settings.value("discordShowNoteTitle", true).toBool();
        discordClientId = settings.value("discordClientId").toString().trimmed();
        discordPresence.statusChanged = [this](const QString &status) {
            if (discordStatusLabel) discordStatusLabel->setText(status);
        };
        discordPresence.configure(discordEnabled, discordClientId, discordShowTitle);
        QTimer::singleShot(0, this, [this] { refreshUsage(usageProvider); });
        roots = settings.value("folders").toStringList();
        if (!initial.isEmpty()) {
            const QFileInfo entry(initial);
            if (entry.isDir()) addRoot(entry.absoluteFilePath());
            else if (entry.isFile()) addRoot(entry.absolutePath());
        }
        reloadTree();
        showEmpty();
        for (const auto &file : settings.value("openTabs").toStringList()) {
            if (fileItems.contains(file)) addTab(file);
        }
        const QFileInfo entry(initial);
        const QString last = entry.isFile() ? entry.absoluteFilePath() : settings.value("note").toString();
        if (QFileInfo(last).isFile() && fileItems.contains(last)) open(last);
        else if (tabs->count()) open(tabs->tabData(0).toString());
        else if (!fileItems.isEmpty() && (!initial.isEmpty() || !settings.contains("openTabs"))) {
            const auto paths = fileItems.keys();
            open(*std::min_element(paths.begin(), paths.end()));
        }
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (!saveCurrent()) {
            const auto decision = QMessageBox::question(this, "Unsaved note",
                "Sava could not save this note. Close and discard its edits?",
                QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
            if (decision != QMessageBox::Discard) { event->ignore(); return; }
        }
        discordPresence.shutdown();
        QSettings settings;
        settings.setValue("folders", roots);
        settings.setValue("note", current);
        QStringList openTabs;
        for (int index = 0; index < tabs->count(); ++index)
            openTabs.append(tabs->tabData(index).toString());
        settings.setValue("openTabs", openTabs);
        event->accept();
    }

private:
    int sidebarWidth() const { return qBound(142, qRound(width() * .16), 200); }

    void applyTextSize(int size)
    {
        textSize = size;
        QFont font = source->font();
        font.setPointSize(size);
        source->setFont(font);
        if (preview) preview->setFont(font);
    }

    void updateDiscordPresence()
    {
        if (current.isEmpty()) {
            discordPresence.setActivity({}, {}, 0, false);
            return;
        }
        const QFileInfo info(current);
        discordPresence.setActivity(info.completeBaseName(), info.dir().dirName(),
                                    currentWordCount, mode && mode->previewing());
    }

    void applyDiscordSettings()
    {
        QSettings settings;
        settings.setValue("discordPresenceEnabled", discordEnabled);
        settings.setValue("discordShowNoteTitle", discordShowTitle);
        settings.setValue("discordClientId", discordClientId);
        discordPresence.configure(discordEnabled, discordClientId, discordShowTitle);
        updateDiscordPresence();
    }

    void makeAppSettings()
    {
        settingsPage = new QWidget(pages);
        settingsPage->setObjectName("appSettings");
        auto *page = new QVBoxLayout(settingsPage);
        page->setContentsMargins(24, 22, 24, 22);
        auto *content = new QWidget(settingsPage);
        content->setFixedWidth(460);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        auto *heading = new QHBoxLayout;
        auto *title = new QLabel("Settings", content);
        title->setObjectName("settingsTitle");
        heading->addWidget(title);
        heading->addStretch();
        settingsBack = tool("←  Back", "Back to workspace", content);
        heading->addWidget(settingsBack);
        layout->addLayout(heading);
        connect(settingsBack, &QToolButton::clicked, this, [this] { showAppSettings(false); });

        auto *editor = new QFrame(content);
        editor->setProperty("settingsGroup", true);
        auto *fields = new QVBoxLayout(editor);
        fields->setContentsMargins(14, 12, 14, 12);
        fields->setSpacing(9);
        auto addNumber = [editor, fields](const QString &name, int minimum, int maximum, int value) {
            auto *row = new QHBoxLayout;
            auto *label = new QLabel(name, editor);
            row->addWidget(label);
            row->addStretch();
            auto *input = new QSpinBox(editor);
            input->setObjectName("settingValue");
            input->setButtonSymbols(QAbstractSpinBox::NoButtons);
            input->setKeyboardTracking(false);
            input->setRange(minimum, maximum);
            input->setValue(value);
            input->setMinimumWidth(100);
            label->setBuddy(input);
            row->addWidget(input);
            fields->addLayout(row);
            return input;
        };
        auto *size = addNumber("Writing text size", 10, 24, textSize);
        size->setSuffix(" pt");
        connect(size, &QSpinBox::valueChanged, this, [this](int value) {
            applyTextSize(value);
            QSettings().setValue("editorTextSize", value);
        });
        layout->addWidget(editor);

        auto *agent = new QFrame(content);
        agent->setProperty("settingsGroup", true);
        auto *agentRow = new QHBoxLayout(agent);
        agentRow->setContentsMargins(14, 12, 14, 12);
        agentRow->addWidget(new QLabel("Co-write providers", agent), 1);
        auto *configure = new QPushButton("Model && effort", agent);
        configure->setObjectName("openAgentSettings");
        agentRow->addWidget(configure);
        layout->addWidget(agent);
        connect(configure, &QPushButton::clicked, this, [this] { openProviderSettings(); });

        auto *discord = new QFrame(content);
        discord->setProperty("settingsGroup", true);
        auto *discordLayout = new QVBoxLayout(discord);
        discordLayout->setContentsMargins(14, 12, 14, 12);
        discordLayout->setSpacing(9);

        auto *discordHeader = new QHBoxLayout;
        auto *discordTitle = new QLabel("Discord Rich Presence", discord);
        discordTitle->setObjectName("settingsSection");
        discordHeader->addWidget(discordTitle);
        discordHeader->addStretch();
        discordStatusLabel = new QLabel(discordPresence.statusText(), discord);
        discordStatusLabel->setObjectName("settingsHint");
        discordHeader->addWidget(discordStatusLabel);
        discordLayout->addLayout(discordHeader);

        auto *enableDiscord = new QCheckBox("Share writing activity on Discord", discord);
        enableDiscord->setChecked(discordEnabled);
        enableDiscord->setCursor(Qt::PointingHandCursor);
        discordLayout->addWidget(enableDiscord);

        auto *showTitleBox = new QCheckBox("Show current note title", discord);
        showTitleBox->setChecked(discordShowTitle);
        showTitleBox->setEnabled(discordEnabled);
        showTitleBox->setCursor(Qt::PointingHandCursor);
        discordLayout->addWidget(showTitleBox);

        auto *appIdRow = new QHBoxLayout;
        auto *appIdLabel = new QLabel("Application ID", discord);
        auto *appIdInput = new QLineEdit(discordClientId, discord);
        appIdInput->setObjectName("settingInput");
        appIdInput->setPlaceholderText("Discord Developer Portal App ID");
        appIdLabel->setBuddy(appIdInput);
        appIdRow->addWidget(appIdLabel);
        appIdRow->addWidget(appIdInput, 1);
        discordLayout->addLayout(appIdRow);

        auto *discordHint = new QLabel(
            QStringLiteral("Create an app at discord.com/developers/applications, upload sava.png as "
                            "Rich Presence art asset \"%1\", and paste its Application ID here "
                            "(or set SAVA_DISCORD_CLIENT_ID).")
                .arg(QLatin1String(sava::kDiscordPresenceImageAsset)),
            discord);
        discordHint->setObjectName("settingsHint");
        discordHint->setWordWrap(true);
        discordLayout->addWidget(discordHint);

        connect(enableDiscord, &QCheckBox::toggled, this, [this, showTitleBox](bool checked) {
            discordEnabled = checked;
            showTitleBox->setEnabled(checked);
            applyDiscordSettings();
        });
        connect(showTitleBox, &QCheckBox::toggled, this, [this](bool checked) {
            discordShowTitle = checked;
            applyDiscordSettings();
        });
        connect(appIdInput, &QLineEdit::editingFinished, this, [this, appIdInput] {
            const QString id = appIdInput->text().trimmed();
            if (id == discordClientId) return;
            discordClientId = id;
            applyDiscordSettings();
        });
        layout->addWidget(discord);

        page->addWidget(content, 0, Qt::AlignTop | Qt::AlignHCenter);
        page->addStretch();
        pages->addWidget(settingsPage);
        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), settingsPage);
        escape->setContext(Qt::WidgetWithChildrenShortcut);
        connect(escape, &QShortcut::activated, this, [this] { showAppSettings(false); });
    }

    void showAppSettings(bool open)
    {
        const bool changing = open != (pages->currentWidget() == settingsPage && settingsPage);
        if (open && !settingsPage) makeAppSettings();
        settingsButton->setChecked(open);
        statusBar()->setVisible(!open);
        if (open) {
            pages->setCurrentWidget(settingsPage);
            settingsBack->setFocus();
            if (changing) sava::fadeIn(settingsPage);
        }
        else if (pages->currentWidget() != panes) {
            pages->setCurrentWidget(panes);
            sava::fadeIn(panes);
            settingsButton->setFocus();
        }
    }

    void openProviderSettings()
    {
        showAppSettings(false);
        star->setChecked(true);
        const auto open = [this] {
            if (star->isChecked() && pages->currentWidget() == panes) chat->openSettings();
        };
        if (chatSlide && chatSlide->state() == QAbstractAnimation::Running)
            connect(chatSlide, &QVariantAnimation::finished, this, open, Qt::SingleShotConnection);
        else
            QTimer::singleShot(0, this, open);
    }

    void makeFiles()
    {
        files = new QWidget(panes);
        files->setObjectName("rail");
        files->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        auto *layout = new QVBoxLayout(files);
        layout->setContentsMargins(9, 7, 9, 10);
        auto *controls = new QHBoxLayout;
        controls->addStretch();
        auto *folder = iconTool(attachFolderIcon(), "Attach folder", files);
        auto *fresh = iconTool(crossIcon(false), "New note", files);
        auto *hide = iconTool(sidebarIcon(), "Hide sidebar", files);
        controls->addWidget(folder);
        controls->addWidget(fresh);
        controls->addWidget(hide);
        controls->addStretch();
        layout->addLayout(controls);
        connect(folder, &QToolButton::clicked, this, [this] { attach(); });
        connect(fresh, &QToolButton::clicked, this, [this] { create(); });
        connect(hide, &QToolButton::clicked, this, [this] {
            showFiles->show();
            slidePane(files, false, sidebarWidth(), fileSlide);
            showFiles->setFocus();
        });
        search = new QLineEdit(files);
        search->setPlaceholderText("Search…");
        search->setClearButtonEnabled(true);
        layout->addWidget(search);
        tree = new FileTree(files);
        tree->setHeaderHidden(true);
        tree->setIndentation(15);
        tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tree->setUniformRowHeights(true);
        layout->addWidget(tree, 1);
        connect(search, &QLineEdit::textChanged, this, [this] { filterFiles(); });
        tree->rowClicked = [this](QTreeWidgetItem *item) {
            const QString file = item->data(0, Qt::UserRole).toString();
            if (file.isEmpty())
                item->setExpanded(!item->isExpanded());
            else
                open(file, false);
        };
        tree->menuRequested = [this](QTreeWidgetItem *item, const QPoint &position) {
            showFileMenu(item, position);
        };
        for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            auto *activate = new QShortcut(QKeySequence(key), tree);
            activate->setContext(Qt::WidgetShortcut);
            connect(activate, &QShortcut::activated, this, [this] {
                const auto notes = selectedNotes();
                if (!notes.isEmpty())
                    openNotes(notes);
                else if (auto *item = tree->currentItem(); item && item->isSelected())
                    if (item->data(0, Qt::UserRole).toString().isEmpty())
                        item->setExpanded(!item->isExpanded());
            });
        }
        auto *remove = new QShortcut(QKeySequence(Qt::Key_Delete), tree);
        remove->setContext(Qt::WidgetShortcut);
        remove->setAutoRepeat(false);
        connect(remove, &QShortcut::activated, this, [this] { trashNotes(selectedNotes()); });

        auto addMeter = [this, layout](QWidget *label, QLabel *&value, QProgressBar *&bar,
                                       const QString &name) {
            auto *line = new QHBoxLayout;
            line->addWidget(label);
            line->addStretch();
            value = new QLabel("—", files);
            line->addWidget(value);
            layout->addLayout(line);
            bar = new QProgressBar(files);
            bar->setAccessibleName(name);
            bar->setFixedHeight(5);
            bar->setTextVisible(false);
            bar->setValue(0);
            layout->addWidget(bar);
        };
        usageButton = tool("Usage", "Refresh provider usage", files);
        usageButton->setStyleSheet("QToolButton { padding: 0 2px 0 0; }");
        connect(usageButton, &QToolButton::clicked, this, [this] { refreshUsage(usageProvider); });
        addMeter(usageButton, usageText, usageBar, "Provider usage");
        layout->addSpacing(4);
        addMeter(new QLabel("Context window", files), contextText, contextBar, "Context window");
        contextBar->setObjectName("context");
        contextBar->setToolTip("Estimated tokens in the note and conversation.");
    }

    void makeEditor()
    {
        auto *editor = new QWidget(panes);
        auto *layout = new QVBoxLayout(editor);
        layout->setContentsMargins(17, 8, 17, 0);
        auto *toolbar = new QHBoxLayout;
        tabs = new QTabBar(editor);
        tabs->setObjectName("noteTabs");
        tabs->setAccessibleName("Open notes");
        tabs->setDocumentMode(true);
        tabs->setDrawBase(false);
        tabs->setUsesScrollButtons(true);
        tabs->setElideMode(Qt::ElideRight);
        tabs->setExpanding(false);
        tabs->setMovable(true);
        tabs->setIconSize(QSize(9,9));
        toolbar->addWidget(tabs,0);
        auto *fresh = iconTool(crossIcon(false), "New note", editor);
        toolbar->addWidget(fresh);
        toolbar->addStretch();
        mode = new sava::ModeSelector(editor);
        mode->changed = [this](bool previewing) { showPreview(previewing); };
        toolbar->addWidget(mode);
        layout->addLayout(toolbar);
        layout->addSpacing(7);
        path = new QLabel(editor);
        path->setObjectName("path");
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(path);
        surfaces = new QStackedWidget(editor);
        source = new sava::WritingArea(surfaces);
#ifdef Q_OS_WIN
        source->setFont(QFont(QStringLiteral("Segoe UI"), 12));
#else
        source->setFont(QFont("Sans Serif", 12));
#endif
        source->document()->setDocumentMargin(12);
        new NoteStyle(source->document());
        source->setTabChangesFocus(false);
        surfaces->addWidget(source);
        layout->addWidget(surfaces, 1);
        connect(fresh, &QToolButton::clicked, this, [this] { create(); });
        connect(tabs, &QTabBar::currentChanged, this, [this](int index) {
            if (index < 0) return;
            open(tabs->tabData(index).toString());
            selectTab();
        });
        connect(new QShortcut(QKeySequence("Ctrl+W"), editor), &QShortcut::activated, this, [this] {
            if (pages->currentWidget() == panes) closeTab(tabs->currentIndex());
        });
        connect(new QShortcut(QKeySequence("Ctrl+Tab"), editor), &QShortcut::activated, this, [this] {
            if (pages->currentWidget() == panes && tabs->count())
                tabs->setCurrentIndex((tabs->currentIndex() + 1) % tabs->count());
        });
    }

    void showPreview(bool visible)
    {
        mode->setPreview(visible);
        if (!visible) {
            surfaces->setCurrentWidget(source);
            if (preview) preview->clear();
            source->setFocus();
            updateDiscordPresence();
            return;
        }
        if (!preview) {
            preview = new QTextBrowser(surfaces);
            preview->setFont(source->font());
            preview->document()->setDocumentMargin(12);
            preview->setOpenLinks(false);
            surfaces->addWidget(preview);
            connect(preview, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
                if (url.scheme() != "sava") return;
                const QString name = url.path().mid(1);
                for (auto it = fileItems.cbegin(); it != fileItems.cend(); ++it) {
                    if (QFileInfo(it.key()).completeBaseName().compare(name, Qt::CaseInsensitive) == 0) {
                        open(it.key());
                        return;
                    }
                }
                statusBar()->showMessage("Linked note not found: " + name, 3500);
            });
        }
        QString markdown = source->toPlainText();
        static const QRegularExpression wiki(R"(\[\[([^\[\]\r\n]+)\]\])");
        const auto matches = wiki.globalMatch(markdown);
        QString converted;
        qsizetype offset = 0;
        for (const auto &match : matches) {
            converted += markdown.sliced(offset, match.capturedStart() - offset);
            QUrl url;
            url.setScheme("sava");
            url.setPath('/' + match.captured(1));
            converted += '[' + match.captured(1) + "](" + url.toEncoded() + ')';
            offset = match.capturedEnd();
        }
        converted += markdown.sliced(offset);
        auto features = QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub);
        features.setFlag(QTextDocument::MarkdownNoHTML);
        preview->document()->setMarkdown(converted, features);
        surfaces->setCurrentWidget(preview);
        updateDiscordPresence();
    }

    void showEmpty(bool clearExplorerSelection = true)
    {
        current.clear();
        savedText.clear();
        loading = true;
        source->clear();
        loading = false;
        path->setText("Attach a folder to start");
        if (!fileItems.isEmpty()) path->setText("Open a note");
        source->setReadOnly(true);
        source->setPlaceholderText("Notes will appear here.");
        if (clearExplorerSelection) tree->clearSelection();
        showPreview(false);
        updateWords();
        syncChat();
    }

    int tabIndex(const QString &file) const
    {
        for (int index = 0; index < tabs->count(); ++index)
            if (tabs->tabData(index).toString() == file) return index;
        return -1;
    }

    int addTab(const QString &file)
    {
        const int existing = tabIndex(file);
        if (existing >= 0) return existing;
        const QSignalBlocker block(tabs);
        const auto name = QFileInfo(file).completeBaseName();
        const int index = tabs->addTab(name);
        tabs->setTabData(index,file);
        tabs->setTabToolTip(index, QDir::toNativeSeparators(file));
        auto *closeSlot = new QWidget(tabs);
        closeSlot->setObjectName("tabCloseSlot");
        auto *closeLayout = new QHBoxLayout(closeSlot);
        closeLayout->setContentsMargins(0, 0, 7, 0);
        auto *close = tool({}, "Close " + name, closeSlot);
        close->setIcon(crossIcon(true));
        close->setIconSize(QSize(12, 12));
        close->setProperty("tabClose",true);
        close->setFixedSize(20, 20);
        closeLayout->addWidget(close);
        closeSlot->setFixedSize(closeLayout->sizeHint());
        tabs->setTabButton(index, QTabBar::RightSide, closeSlot);
        connect(close, &QToolButton::clicked, this, [this,closeSlot] {
            for (int tab = 0; tab < tabs->count(); ++tab) {
                if (tabs->tabButton(tab, QTabBar::RightSide) == closeSlot) {
                    closeTab(tab);
                    return;
                }
            }
        });
        return index;
    }

    void selectTab()
    {
        const QSignalBlocker block(tabs);
        const int selected = tabIndex(current);
        if (selected >= 0) tabs->setCurrentIndex(selected);
        static const QIcon indicator = tabDot();
        for (int index = 0; index < tabs->count(); ++index)
            tabs->setTabIcon(index,index == selected ? indicator : QIcon{});
    }

    void closeTab(int index)
    {
        if (index < 0 || index >= tabs->count()) return;
        const QString file = tabs->tabData(index).toString();
        if (file == current) {
            if (!saveCurrent()) return;
            if (tabs->count() > 1) {
                const int next = index == tabs->count() - 1 ? index - 1 : index + 1;
                open(tabs->tabData(next).toString());
                if (file == current) return;
            }
        }
        const QSignalBlocker block(tabs);
        tabs->removeTab(index);
        if (file == current) showEmpty();
        tabPositions.remove(file);
        selectTab();
    }

    void addRoot(const QString &dir)
    {
        const auto canonical = QFileInfo(dir).canonicalFilePath();
        if (canonical.isEmpty()) return;
        const bool exists = std::any_of(roots.cbegin(), roots.cend(),
            [&canonical](const QString &entry) { return samePath(entry, canonical); });
        if (!exists) roots.append(canonical);
    }

    void attach()
    {
        const auto folder = QFileDialog::getExistingDirectory(this, "Attach folder",
            roots.isEmpty() ? QDir::homePath() : roots.constLast());
        if (folder.isEmpty()) return;
        addRoot(folder);
        QSettings().setValue("folders", roots);
        reloadTree();
        if (current.isEmpty()) {
            const auto entries = sava::markdownFiles(folder);
            if (!entries.isEmpty()) open(entries.constFirst());
        }
    }

    void filterFiles()
    {
        const QString query = search->text();
        for (auto it = fileItems.cbegin(); it != fileItems.cend(); ++it) {
            const bool match = query.isEmpty() || QFileInfo(it.key()).fileName().contains(query, Qt::CaseInsensitive);
            auto *item = it.value();
            item->setHidden(!match);
            if (!match)
                item->setSelected(false);
            else if (!query.isEmpty())
                for (auto *parent = item->parent(); parent; parent = parent->parent())
                    parent->setExpanded(true);
        }
    }

    QStringList selectedNotes() const
    {
        QStringList notes;
        for (auto *item : tree->selectedItems()) {
            const QString file = item->data(0, Qt::UserRole).toString();
            if (file.isEmpty() || item->isHidden()) continue;
            bool visible = true;
            for (auto *parent = item->parent(); parent; parent = parent->parent()) {
                if (parent->isHidden() || !parent->isExpanded()) {
                    visible = false;
                    break;
                }
            }
            if (visible)
                notes.append(file);
        }
        notes.sort();
        return notes;
    }

    void openNotes(const QStringList &notes)
    {
        if (!saveCurrent()) return;
        for (const auto &file : notes)
            open(file, false);
    }

    void showFileMenu(QTreeWidgetItem *item, const QPoint &position)
    {
        tree->setFocus();
        if (item && !item->isSelected())
            tree->setCurrentItem(item, 0, QItemSelectionModel::ClearAndSelect);

        const QStringList notes = item ? selectedNotes() : QStringList();
        const QString folder = item ? item->data(0, FolderPathRole).toString() : QString();
        QMenu menu(tree);
        QAction *openAction = nullptr;
        QAction *trashAction = nullptr;
        QAction *copyAction = nullptr;
        QAction *renameAction = nullptr;
        QAction *toggleAction = nullptr;
        if (!notes.isEmpty()) {
            openAction = menu.addAction(notes.size() == 1 ? "Open note" : "Open selected notes");
            if (notes.size() == 1 && item && !item->data(0, Qt::UserRole).toString().isEmpty())
                renameAction = menu.addAction("Rename…");
            copyAction = menu.addAction(notes.size() == 1 ? "Copy path" : "Copy paths");
        }
        if (!folder.isEmpty()) {
            toggleAction = menu.addAction(item->isExpanded() ? "Collapse folder" : "Expand folder");
            if (notes.isEmpty()) copyAction = menu.addAction("Copy folder path");
        }
        QAction *createAction = menu.addAction(item ? "New note here" : "New note");
        if (!notes.isEmpty()) {
            menu.addSeparator();
            const QString trash = QLatin1String(kTrashName);
            trashAction = menu.addAction(notes.size() == 1
                ? QString("Move to %1").arg(trash)
                : QString("Move %1 notes to %2").arg(notes.size()).arg(trash));
        }

        QAction *action = menu.exec(position);
        if (!action) return;
        if (action == openAction)
            openNotes(notes);
        else if (action == renameAction)
            renameNote(notes.constFirst());
        else if (action == copyAction) {
            if (notes.isEmpty()) {
                QApplication::clipboard()->setText(QDir::toNativeSeparators(folder));
            } else {
                QStringList nativePaths;
                nativePaths.reserve(notes.size());
                for (const auto &notePath : notes)
                    nativePaths.append(QDir::toNativeSeparators(notePath));
                QApplication::clipboard()->setText(nativePaths.join(QLatin1Char('\n')));
            }
        }
        else if (action == toggleAction)
            item->setExpanded(!item->isExpanded());
        else if (action == createAction) {
            const QString file = item ? item->data(0, Qt::UserRole).toString() : QString();
            create(!folder.isEmpty() || file.isEmpty() ? folder : QFileInfo(file).absolutePath());
        } else if (action == trashAction)
            trashNotes(notes);
    }

    void renameNote(const QString &file)
    {
        const QFileInfo info(file);
        const QString extension = "." + info.suffix();
        QInputDialog dialog(this);
        dialog.setWindowTitle("Rename note");
        dialog.setLabelText("Name without the " + extension + " extension");
        dialog.setTextValue(info.completeBaseName());
        dialog.setOkButtonText("Rename");
        while (dialog.exec() == QDialog::Accepted) {
            const QString name = dialog.textValue().trimmed();
            if (name == info.completeBaseName()) return;
            bool valid = !name.isEmpty() && !name.startsWith('.') &&
                         !name.contains('/') && !name.contains('\\') &&
                         !name.contains(QChar::Null);
#ifdef Q_OS_WIN
            static const QRegularExpression reserved(
                R"(^(?:CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$)|[<>:"|?*\x00-\x1f])",
                QRegularExpression::CaseInsensitiveOption);
            valid = valid && !name.endsWith('.') && !reserved.match(name).hasMatch();
#endif
            if (!valid) {
                dialog.setLabelText("Enter a valid note name without its extension.");
                continue;
            }
            const QString destination = info.dir().filePath(name + extension);
            const QFileInfo sourceInfo(file);
            if (!sourceInfo.isFile() || sourceInfo.isSymLink() ||
                !samePath(sourceInfo.canonicalFilePath(), file)) {
                QMessageBox::warning(this, "Rename note", "The original note is unavailable.");
                return;
            }
            if (file == current && !saveCurrent()) return;
            QFile note(file);
            if (!note.rename(destination)) {
                dialog.setLabelText("Could not rename the note: " + note.errorString());
                continue;
            }

            const int index = tabIndex(file);
            if (index >= 0) {
                tabs->setTabText(index, name);
                tabs->setTabData(index, destination);
                tabs->setTabToolTip(index, QDir::toNativeSeparators(destination));
                if (auto *slot = tabs->tabButton(index, QTabBar::RightSide)) {
                    if (auto *close = slot->findChild<QToolButton *>()) {
                        close->setToolTip("Close " + name);
                        close->setAccessibleName("Close " + name);
                    }
                }
            }
            if (tabPositions.contains(file))
                tabPositions.insert(destination, tabPositions.take(file));
            if (file == current) {
                current = destination;
                updateNotePath(QFileInfo(destination));
            }
            if (chat) chat->renameNote(file, destination, name);
            reloadTree();
            if (auto *item = fileItems.value(destination); item && tree->visualItemRect(item).isValid())
                tree->setCurrentItem(item);
            statusBar()->showMessage("Renamed to " + name + extension, 3500);
            return;
        }
    }

    void trashNotes(const QStringList &notes)
    {
        if (notes.isEmpty() || (notes.contains(current) && !saveCurrent())) return;

        QStringList failures;
        int moved = 0;
        int nextTab = -1;
        {
            const QSignalBlocker block(tabs);
            for (const auto &path : notes) {
                const QFileInfo info(path);
                QFile file(path);
                if (!info.isFile() || info.isSymLink() || !samePath(info.canonicalFilePath(), path)) {
                    failures.append(QString("%1 is missing or is no longer a regular note.")
                        .arg(QDir::toNativeSeparators(path)));
                    continue;
                }
                if (!file.moveToTrash()) {
                    failures.append(QString("%1: %2")
                        .arg(QDir::toNativeSeparators(path), file.errorString()));
                    continue;
                }
                ++moved;
                const int index = tabIndex(path);
                if (path == current) {
                    nextTab = qMax(0, index);
                    autosave.stop();
                } else if (nextTab >= 0 && index >= 0 && index < nextTab) {
                    --nextTab;
                }
                if (index >= 0) tabs->removeTab(index);
                tabPositions.remove(path);
                delete fileItems.take(path);
            }
        }
        if (nextTab >= 0) {
            showEmpty(false);
            if (tabs->count())
                open(tabs->tabData(qMin(nextTab, tabs->count() - 1)).toString(), false);
        }
        selectTab();
        tree->setFocus();
        const QString trash = QLatin1String(kTrashName);
        if (moved)
            statusBar()->showMessage(moved == 1
                ? QString("Moved 1 note to %1").arg(trash)
                : QString("Moved %1 notes to %2").arg(moved).arg(trash), 3500);
        if (!failures.isEmpty()) {
            QMessageBox message(QMessageBox::Warning, QString("Move to %1").arg(trash),
                QString("Could not move %1 of %2 notes to %3.")
                    .arg(failures.size()).arg(notes.size()).arg(trash),
                QMessageBox::Ok, this);
            message.setDetailedText(failures.join(QLatin1Char('\n')));
            message.exec();
        }
    }

    void reloadTree()
    {
        QHash<QString, bool> expanded;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const QString folder = (*it)->data(0, FolderPathRole).toString();
            if (!folder.isEmpty()) expanded.insert(folder, (*it)->isExpanded());
        }
        tree->clear();
        fileItems.clear();
        const auto folderIcon = treeIcon(true, QColor("#ffda70"));
        const auto subfolderIcon = treeIcon(true, QColor("#70cdef"));
        const auto noteIcon = treeIcon(false, QColor("#bcb5ab"));
        for (const auto &root : roots) {
            const QFileInfo rootInfo(root);
            if (!rootInfo.isDir()) continue;
            const QString rootLabel = rootInfo.fileName().isEmpty()
                ? QDir::toNativeSeparators(root)
                : rootInfo.fileName();
            auto *branch = new QTreeWidgetItem(tree, {rootLabel});
            branch->setToolTip(0, QDir::toNativeSeparators(root));
            branch->setIcon(0, folderIcon);
            branch->setData(0, FolderPathRole, root);
            branch->setExpanded(expanded.value(root, true));
            QHash<QString,QTreeWidgetItem*> folders;
            folders.insert(QString(), branch);
            for (const auto &file : sava::markdownFiles(root)) {
                if (fileItems.contains(file)) continue;
                auto parts = QDir(root).relativeFilePath(file).split('/');
                const auto name = parts.takeLast();
                QString location;
                auto *parent = branch;
                for (const auto &part : parts) {
                    location += '/' + part;
                    if (!folders.contains(location)) {
                        auto *dir = new QTreeWidgetItem(parent, {part});
                        const QString folder = QDir::cleanPath(root + location);
                        dir->setIcon(0, subfolderIcon);
                        dir->setData(0, FolderPathRole, folder);
                        dir->setToolTip(0, QDir::toNativeSeparators(folder));
                        dir->setExpanded(expanded.value(folder, false));
                        folders.insert(location, dir);
                    }
                    parent = folders.value(location);
                }
                auto *item = new QTreeWidgetItem(parent, {QFileInfo(name).completeBaseName()});
                item->setIcon(0, noteIcon);
                item->setData(0, Qt::UserRole, file);
                item->setToolTip(0, QDir::toNativeSeparators(file));
                fileItems.insert(file, item);
            }
        }
        filterFiles();
        if (auto *item = fileItems.value(current); item && tree->visualItemRect(item).isValid())
            tree->setCurrentItem(item);
    }

    bool saveCurrent()
    {
        if (current.isEmpty() || !source->document()->isModified()) return true;
        QString error;
        QString onDisk;
        if (!sava::loadNote(current, &onDisk, &error) || onDisk != savedText) {
            statusBar()->showMessage("Could not save. The note changed on disk or is unavailable.");
            return false;
        }
        const auto text = source->toPlainText();
        if (!sava::saveNote(current, text, &error)) {
            statusBar()->showMessage(error);
            return false;
        }
        savedText = text;
        source->document()->setModified(false);
        statusBar()->showMessage("Saved", 1600);
        return true;
    }

    void open(const QString &file, bool selectFile = true)
    {
        if (file == current || !saveCurrent()) return;
        QString text;
        QString error;
        if (!sava::loadNote(file, &text, &error)) {
            statusBar()->showMessage(error);
            return;
        }
        autosave.stop();
        if (highlight) { highlight->stop(); source->setExtraSelections({}); }
        if (!current.isEmpty()) {
            const auto cursor = source->textCursor();
            tabPositions.insert(current,{cursor.anchor(), cursor.position(), source->verticalScrollBar()->value()});
        }
        current = file;
        savedText = text;
        loading = true;
        source->setReadOnly(false);
        source->setPlainText(text);
        loading = false;
        if (const auto it = tabPositions.constFind(file); it != tabPositions.cend()) {
            auto cursor = source->textCursor();
            const int end = source->document()->characterCount() - 1;
            cursor.setPosition(qMin(it->anchor,end));
            cursor.setPosition(qMin(it->position,end),QTextCursor::KeepAnchor);
            source->setTextCursor(cursor);
            source->verticalScrollBar()->setValue(it->scroll);
        }
        const QFileInfo info(file);
        addTab(file);
        selectTab();
        updateNotePath(info);
        if (selectFile) {
            if (auto *item = fileItems.value(file); item && !item->isHidden()) {
                for (auto *parent = item->parent(); parent; parent = parent->parent())
                    parent->setExpanded(true);
                tree->setCurrentItem(item);
            }
        }
        if (mode->previewing()) showPreview(true);
        updateWords();
        syncChat();
    }

    void updateNotePath(const QFileInfo &info)
    {
        const QString dirLabel = info.dir().dirName().isEmpty()
            ? QDir::toNativeSeparators(info.absolutePath())
            : info.dir().dirName();
        path->setText(dirLabel + " / " + info.fileName());
    }

    void create(const QString &folder = {})
    {
        if (!saveCurrent()) return;
        if (roots.isEmpty()) attach();
        if (roots.isEmpty()) return;
        QString root = folder;
        if (root.isEmpty()) {
            root = roots.constLast();
            for (const auto &candidate : roots) {
                if (pathUnderRoot(current, candidate)) { root = candidate; break; }
            }
        }
        QString error;
        const auto file = sava::createNote(root, &error);
        if (file.isEmpty()) { statusBar()->showMessage(error); return; }
        search->clear();
        reloadTree();
        open(file);
        showPreview(false);
        if (current == file && source->document()->isEmpty()) {
            auto cursor = source->textCursor();
            cursor.insertText("# " + QFileInfo(file).completeBaseName() + "\n\n");
            source->setTextCursor(cursor);
            saveCurrent();
        }
    }

    void updateWords()
    {
        const auto text = source->toPlainText();
        qsizetype count = 0;
        bool inWord = false;
        for (const QChar character : text) {
            const bool word = !character.isSpace();
            if (word && !inWord) ++count;
            inWord = word;
        }
        currentWordCount = count;
        words->setText(QString::number(count) + (count == 1 ? " word" : " words"));
        if (!chat)
            updateContext(text.isEmpty() ? -1 : (text.size() + 3) / 4, -1);
        updateDiscordPresence();
    }

    void updateContext(qint64 estimated, qint64 limit)
    {
        int percent = 0;
        QString label = "—";
        if (estimated >= 0 && limit > 0) {
            percent = static_cast<int>(100.0 * qMin(estimated, limit) / limit);
            label = QString::number(percent) + '%';
        } else if (estimated >= 0) {
            label = "~" + QString::number(estimated);
        }
        contextBar->setValue(percent);
        contextText->setText(label);
    }

    void refreshUsage(sava::Harness provider)
    {
        if (provider != usageProvider) {
            usageText->setText("—");
            usageBar->setValue(0);
        }
        usageProvider = provider;
        const QString name = sava::Agents::displayName(provider);
        const bool supported = provider == sava::Harness::Codex || provider == sava::Harness::Claude;
        usageButton->setEnabled(supported);
        if (supported) {
            usageText->setToolTip(QString("Checking %1 quota…").arg(name));
            usageBar->setToolTip(usageText->toolTip());
        }
        quotaCatalog.readQuota(provider, [this, name, supported](const sava::ProviderQuota &quota) {
            const auto &display = quota.primary.usedPercent >= 0 ? quota.primary : quota.secondary;
            usageText->setText(display.usedPercent >= 0 ? quotaPercent(display.usedPercent) : QString("—"));
            usageBar->setValue(qRound(qBound(0.0, display.usedPercent, 100.0)));
            QString details = QString("%1 account usage").arg(name);
            const QLocale locale;
            for (const auto &window : {quota.primary, quota.secondary}) {
                if (window.usedPercent >= 0)
                    details += '\n' + quotaDetails(window, locale);
            }
            if (display.usedPercent < 0) {
                details += supported ? "\nNo quota reported. Click Usage to retry."
                                     : "\nThis harness does not report an account quota.";
            }
            usageText->setToolTip(details);
            usageBar->setToolTip(details);
        });
    }

    void setChatVisible(bool visible)
    {
        if (visible && !chat) {
            chat = new sava::ChatPane(panes);
            chat->setMinimumWidth(0);
            chat->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
            chat->setMaximumWidth(420);
            chat->insertText = [this](const QString &text) {
                if (current.isEmpty()) return;
                showPreview(false);
                auto cursor = source->textCursor();
                const auto contents = source->toPlainText();
                cursor.movePosition(QTextCursor::End);
                const QString separator = contents.isEmpty() || contents.endsWith("\n\n") ? QString()
                    : contents.endsWith('\n') ? QString("\n") : QString("\n\n");
                cursor.insertText(separator + text);
                source->setTextCursor(cursor);
            };
            chat->jumpToPassage = [this](const QString &text) { jumpTo(text); };
            chat->contextUpdated = [this](qint64 estimated, qint64 limit) {
                updateContext(estimated, limit);
            };
            chat->providerUsageChanged = [this](sava::Harness provider) { refreshUsage(provider); };
            chat->hide();
        }
        if (chat) {
            if (visible) syncChat();
            slidePane(chat, visible, qBound(188, qRound(width() * .23), 294), chatSlide);
        }
    }

    void slidePane(QWidget *pane, bool open, int width, QPointer<QVariantAnimation> &motion)
    {
        if (motion) {
            motion->stop();
            motion->deleteLater();
        }
        const int index = panes->indexOf(pane);
        const int start = pane->isHidden() ? 0 : panes->sizes().at(index);
        pane->show();
        auto resizePane = [this,index](int requested) {
            auto sizes = panes->sizes();
            const int available = sizes.at(1) + sizes.at(index);
            const int size = qBound(0, requested, available);
            sizes[index] = size;
            sizes[1] = available - size;
            panes->setSizes(sizes);
        };
        resizePane(start);
        motion = new QVariantAnimation(this);
        motion->setDuration(250);
        motion->setStartValue(start);
        motion->setEndValue(open ? width : 0);
        motion->setEasingCurve(QEasingCurve::InOutCubic);
        connect(motion, &QVariantAnimation::valueChanged, this,
                [resizePane](const QVariant &value) { resizePane(value.toInt()); });
        connect(motion, &QVariantAnimation::finished, pane, [pane,open] {
            if (!open) pane->hide();
        });
        motion->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void syncChat()
    {
        if (chat) chat->setNote(current, QFileInfo(current).completeBaseName(), source->toPlainText());
    }

    void jumpTo(const QString &passage)
    {
        if (passage.trimmed().isEmpty()) return;
        showPreview(false);
        const auto match = source->document()->find(passage.trimmed());
        if (match.isNull()) { statusBar()->showMessage("Passage not found in this note", 3000); return; }
        auto caret = match;
        caret.clearSelection();
        source->setTextCursor(caret);
        source->centerCursor();
        if (highlight) { highlight->stop(); highlight->deleteLater(); }
        highlight = new QVariantAnimation(this);
        highlight->setDuration(1500);
        highlight->setStartValue(0.0);
        highlight->setEndValue(1.0);
        connect(highlight, &QVariantAnimation::valueChanged, this, [this,match](const QVariant &value) {
            const auto progress = value.toReal();
            const auto filled = QEasingCurve(QEasingCurve::InOutCubic)
                                    .valueForProgress(qMin(1.0, progress / .32));
            const auto alpha = qRound(110 * (1 - qBound(0.0, (progress - .72) / .28, 1.0)));
            QTextEdit::ExtraSelection chosen;
            chosen.cursor = match;
            chosen.cursor.setPosition(match.selectionStart());
            chosen.cursor.setPosition(match.selectionStart() + qRound(filled * (match.selectionEnd() - match.selectionStart())),
                                      QTextCursor::KeepAnchor);
            chosen.format.setBackground(QColor(255,128,167,alpha));
            source->setExtraSelections({chosen});
        });
        connect(highlight, &QVariantAnimation::finished, this, [this] { source->setExtraSelections({}); });
        highlight->start();
    }

    QStackedWidget *pages = nullptr;
    QWidget *settingsPage = nullptr;
    QToolButton *settingsButton = nullptr;
    QToolButton *settingsBack = nullptr;
    StarButton *star = nullptr;
    QSplitter *panes = nullptr;
    QWidget *files = nullptr;
    QToolButton *showFiles = nullptr;
    sava::ModeSelector *mode = nullptr;
    FileTree *tree = nullptr;
    QLineEdit *search = nullptr;
    QTabBar *tabs = nullptr;
    QLabel *path = nullptr;
    QLabel *words = nullptr;
    QLabel *usageText = nullptr;
    QToolButton *usageButton = nullptr;
    QLabel *contextText = nullptr;
    QProgressBar *contextBar = nullptr;
    QProgressBar *usageBar = nullptr;
    QStackedWidget *surfaces = nullptr;
    QPlainTextEdit *source = nullptr;
    QTextBrowser *preview = nullptr;
    sava::ChatPane *chat = nullptr;
    QVariantAnimation *highlight = nullptr;
    QPointer<QVariantAnimation> fileSlide;
    QPointer<QVariantAnimation> chatSlide;
    QTimer autosave;
    QStringList roots;
    QHash<QString,QTreeWidgetItem*> fileItems;
    struct TabPosition { int anchor; int position; int scroll; };
    QHash<QString,TabPosition> tabPositions;
    QString current;
    QString savedText;
    sava::ModelCatalog quotaCatalog;
    sava::DiscordPresence discordPresence;
    QLabel *discordStatusLabel = nullptr;
    sava::Harness usageProvider = sava::Harness::Codex;
    QString discordClientId;
    qsizetype currentWordCount = 0;
    int textSize = 12;
    bool discordEnabled = false;
    bool discordShowTitle = true;
    bool loading = false;
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName("Sava");
    app.setApplicationName("Sava");
    app.setStyle("Fusion");
#ifdef Q_OS_WIN
    app.setFont(QFont(QStringLiteral("Segoe UI"), 10));
#endif
    app.setWindowIcon(QIcon(QStringLiteral(":/sava.png")));
    app.setStyleSheet(QString::fromLatin1(style));
    ButtonCursors buttonCursors;
    app.installEventFilter(&buttonCursors);
    const auto args = app.arguments();
    Window window(args.size() > 1 ? args.at(1) : QString());
    applyWindowsTitleBar(&window);
    window.show();
    return app.exec();
}
