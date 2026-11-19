#include "chat.h"

#include "agents.h"
#include "app-icons.h"
#include "editor-controls.h"
#include "model-catalog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QButtonGroup>
#include <QComboBox>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QMimeData>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <memory>

namespace sava {
namespace {

const QColor kInk("#171717");
const QColor kRail("#1d1d1d");
const QColor kEdge("#353535");
const QColor kPaper("#f3e9d5");
const QColor kSoft("#c0bbb3");
const QColor kQuiet("#a49f96");
const QColor kPink("#ff80a7");
const QColor kGreen("#76e3b4");
const QColor kBlue("#70cdef");

constexpr int kMaxMessages = 40;
constexpr int kMaxNotes = 24;
constexpr int kMaxInputChars = 2000;
constexpr int kMaxNoteChars = 4000;
constexpr int kMaxHistoryMessages = 6;
constexpr int kMaxHistoryChars = 3000;
constexpr int kMaxImages = 3;
constexpr int kImageSide = 1600;
constexpr int kMaxImageBytes = 2 * 1024 * 1024;

const Harness kHarnesses[] = {
    Harness::Codex, Harness::Claude, Harness::OpenCode, Harness::Pi, Harness::Antigravity
};

enum class Mode { Solo, Council };
enum class Phase { Idle, Solo, Council };

struct Settings {
    QString model = QStringLiteral("Default");
    QString effort = QStringLiteral("Default");
};

struct ChatImage {
    std::shared_ptr<QTemporaryFile> file;
    QPixmap thumbnail;
};

struct ReplyQuote {
    quint64 id = 0;
    QString speaker;
    QString text;
};

struct Message {
    quint64 id = 0;
    bool user = false;
    bool notice = false;
    bool error = false;
    QString text;
    Harness harness = Harness::Codex;
    QString model;
    QString effort;
    QString jumpPassage;
    QString insertSuggestion;
    bool insertIsLine = false;
    bool inserted = false;
    bool hint = false;
    ReplyQuote quote;
    QList<ChatImage> images;
};

struct Conversation {
    QVector<Message> messages;
};

struct NoteState {
    Conversation solo;
    Conversation council;
    qint64 contextLimit = -1;
};

struct Run {
    Phase phase = Phase::Idle;
    Mode mode = Mode::Solo;
    QString noteId;
    quint64 cycle = 0;
    Harness harness = Harness::Codex;
    QVector<Harness> order;
    int position = 0;
    bool webSearch = false;
    QList<ChatImage> images;
};

QString harnessName(Harness harness)
{
    const QString name = Agents::displayName(harness);
    if (!name.isEmpty())
        return name;
    switch (harness) {
    case Harness::Codex:
        return QStringLiteral("Codex");
    case Harness::Claude:
        return QStringLiteral("Claude Code");
    case Harness::OpenCode:
        return QStringLiteral("OpenCode");
    case Harness::Pi:
        return QStringLiteral("Pi");
    case Harness::Antigravity:
        return QStringLiteral("Antigravity");
    }
    return QStringLiteral("Agent");
}

bool isConcreteModel(const QString &model)
{
    const QString value = model.trimmed();
    return !value.isEmpty() && value.compare(QStringLiteral("default"), Qt::CaseInsensitive) != 0;
}

qint64 estimateTokens(const QString &text)
{
    return (qint64(text.size()) + 3) / 4;
}

class ReplyPulse final : public QWidget
{
public:
    ReplyPulse()
    {
        setFixedSize(24, 14);
        timer.setInterval(40);
        connect(&timer, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, this) > 0) {
            elapsed.start();
            timer.start();
        }
    }

    void hideEvent(QHideEvent *event) override
    {
        timer.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const double phase = timer.isActive() ? (elapsed.elapsed() % 1200) / 1200.0 : 0.0;
        for (int i = 0; i < 3; ++i) {
            const double pulse = timer.isActive()
                ? (1.0 + std::cos(6.283185307 * (phase - i * 0.15))) / 2.0 : 0.5;
            QColor color = kBlue;
            color.setAlphaF(0.4 + 0.6 * pulse);
            painter.setBrush(color);
            painter.drawEllipse(QPointF(4 + i * 8, 8 - 2 * pulse), 2.2, 2.2);
        }
    }

private:
    QTimer timer;
    QElapsedTimer elapsed;
};

QString bounded(const QString &text, int max)
{
    if (max <= 0)
        return {};
    if (text.size() <= max)
        return text;
    return text.left(max - 1) + QChar(0x2026);
}

QString normalizedText(QString text)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    text.replace(whitespace, QStringLiteral(" "));
    return text.trimmed();
}

QVector<QString> quotedSegments(const QString &text)
{
    static const QRegularExpression quotes(
        QStringLiteral("[\u201C\"]([^\u201D\"\\n]{6,240})[\u201D\"]"));
    QVector<QString> result;
    auto match = quotes.globalMatch(text);
    while (match.hasNext()) {
        const QString value = match.next().captured(1).trimmed();
        if (!value.isEmpty() && !result.contains(value))
            result.append(value);
    }
    return result;
}

void deriveActions(Message &message, const QString &noteText)
{
    const QVector<QString> quotes = quotedSegments(message.text);
    const QString note = normalizedText(noteText);
    for (const QString &quote : quotes) {
        if (quote.size() >= 8 && note.contains(normalizedText(quote), Qt::CaseInsensitive)) {
            message.jumpPassage = quote;
            break;
        }
    }
    for (const QString &quote : quotes) {
        if (quote != message.jumpPassage) {
            message.insertSuggestion = quote;
            message.insertIsLine = true;
            break;
        }
    }
    if (message.insertSuggestion.isEmpty()) {
        static const QRegularExpression suggestion(
            QStringLiteral("(?:^|[.!?]\\s+)((?:Try|Consider|Open with|Start with)\\s+[^.!?]{8,240}[.!?])"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = suggestion.match(message.text);
        if (match.hasMatch()) {
            message.insertSuggestion = match.captured(1).trimmed();
            message.insertIsLine = false;
        }
    }
    if (message.insertSuggestion.isEmpty())
        message.insertSuggestion = bounded(message.text.trimmed(), 600);
}

class ProviderButton : public QAbstractButton {
public:
    ProviderButton(Harness harness, QWidget *parent = nullptr)
        : QAbstractButton(parent)
        , m_harness(harness)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFixedSize(25, 24);
        setToolTip(harnessName(harness));
        setAccessibleName(harnessName(harness));
    }

    Harness harness() const { return m_harness; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect());
        if (isEnabled() && (underMouse() || isDown())) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, isDown() ? 42 : 27));
            painter.drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
        }
        const QPixmap icon =
            harnessIcon(harnessName(m_harness)).pixmap(QSize(15, 15), devicePixelRatioF());
        const int x = (rect().width() - 15) / 2;
        const int y = (rect().height() - 15) / 2 - 1;
        if (!icon.isNull()) {
            painter.setOpacity(isChecked() || underMouse() ? 1.0 : 0.66);
            painter.drawPixmap(x, y, icon);
            painter.setOpacity(1.0);
        }
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(kBlue, 1.4));
            painter.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 7, 7);
        }
    }

private:
    Harness m_harness;
};

class SliderButton : public QAbstractButton {
public:
    explicit SliderButton(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFixedSize(27, 26);
        setToolTip(QStringLiteral("Model and effort"));
        setAccessibleName(QStringLiteral("Model and effort"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect());
        if (underMouse() || isDown() || isChecked()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, underMouse() ? 29 : 14));
            painter.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 7, 7);
        }
        const QColor color = (isChecked() || underMouse()) ? kBlue : kSoft;
        const qreal rows[3] = {9.0, 13.0, 17.0};
        const qreal knobs[3] = {13.0, 9.0, 16.0};
        for (int index = 0; index < 3; ++index) {
            painter.setPen(QPen(color, 1.2));
            painter.drawLine(QPointF(7.0, rows[index]), QPointF(20.0, rows[index]));
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(QPointF(knobs[index], rows[index]), 1.5, 1.5);
        }
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(kBlue, 1.3));
            painter.drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 7, 7);
        }
    }
};

class CompactCombo : public QComboBox {
public:
    explicit CompactCombo(QWidget *parent = nullptr)
        : QComboBox(parent)
        , options(new QMenu(this))
    {
        options->setObjectName(QStringLiteral("savaDropdown"));
        options->setAttribute(Qt::WA_TranslucentBackground);
        options->setWindowFlag(Qt::NoDropShadowWindowHint);
        options->setStyleSheet(QStringLiteral(
            "QMenu#savaDropdown{background:#252525;color:#f3e9d5;border:1px solid #44413e;"
            "border-radius:10px;padding:4px;font-size:12px;}"
            "QMenu#savaDropdown::item{padding:6px 12px 6px 23px;border-radius:5px;}"
            "QMenu#savaDropdown::item:selected{background:#263740;color:#f3e9d5;}"
            "QMenu#savaDropdown::indicator{width:6px;height:6px;margin-left:8px;"
            "border:0;border-radius:3px;background:transparent;image:none;}"
            "QMenu#savaDropdown::indicator:checked{background:#76e3b4;}"));
        connect(options, &QMenu::triggered, this, [this](QAction *action) {
            if (!action->data().isValid()) return;
            const int index = action->data().toInt();
            setCurrentIndex(index);
            emit activated(index);
        });
        connect(options, &QMenu::aboutToHide, this, [this] {
            QComboBox::hidePopup();
            QTimer::singleShot(0, options, [this] {
                if (!options->isVisible()) clearOptions();
            });
        });
    }

    void showPopup() override
    {
        if (!isEnabled() || count() == 0) return;
        clearOptions();
        QHash<QString,QMenu *> groups;
        QAction *active = nullptr;
        for (int index = 0; index < count(); ++index) {
            const QString text = itemText(index);
            const auto separator = text.indexOf('/');
            QMenu *menu = options;
            if (count() > 25 && separator > 0) {
                const QString group = text.left(separator);
                menu = groups.value(group);
                if (!menu) {
                    menu = options->addMenu(group);
                    menu->setObjectName(QStringLiteral("savaDropdown"));
                    menu->setAttribute(Qt::WA_TranslucentBackground);
                    menu->setWindowFlag(Qt::NoDropShadowWindowHint);
                    groups.insert(group,menu);
                }
            }
            auto *option = menu->addAction(menu == options ? text : text.sliced(separator + 1));
            option->setData(index);
            option->setCheckable(true);
            if (index == currentIndex()) {
                option->setChecked(true);
                active = menu == options ? option : menu->menuAction();
            }
        }
        options->setMinimumWidth(width());
        const QPoint below = mapToGlobal(QPoint(0, height() + 4));
        QPoint position = below;
        if (const auto *screen = QGuiApplication::screenAt(below)) {
            const auto available = screen->availableGeometry();
            const auto size = options->sizeHint();
            position.setX(qBound(available.left(), position.x(),
                                 qMax(available.left(), available.right() - size.width() + 1)));
            if (below.y() + size.height() > available.bottom() + 1) {
                const int above = mapToGlobal(QPoint(0, -size.height() - 4)).y();
                position.setY(qMax(available.top(), above));
            }
        }
        options->popup(position);
        if (active) options->setActiveAction(active);
    }

    void hidePopup() override
    {
        options->hide();
        QComboBox::hidePopup();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(isEnabled() ? kSoft : QColor("#77736c"), 1.2));
        const QPointF center(width() - 10.0, height() / 2.0);
        painter.drawLine(center + QPointF(-3.0, -1.0), center + QPointF(0.0, 2.0));
        painter.drawLine(center + QPointF(0.0, 2.0), center + QPointF(3.0, -1.0));
    }

private:
    void clearOptions()
    {
        qDeleteAll(options->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
        options->clear();
    }

    QMenu *options;
};

class SettingsPopup : public QFrame {
public:
    explicit SettingsPopup(QWidget *parent = nullptr)
        : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setObjectName(QStringLiteral("chatSettings"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QStringLiteral(
            "QFrame#chatSettings{background:#252525;border:1px solid #41403d;border-radius:11px;}"
            "QLabel{background:transparent;color:#c0bbb3;font-size:12px;}"
            "QLabel#chatSettingsTitle{color:#f3e9d5;font-size:12px;font-weight:700;}"
            "QComboBox{background:#252525;border:1px solid #353535;border-radius:6px;"
            "padding:3px 16px 3px 7px;color:#f3e9d5;font-size:12px;}"
            "QComboBox:focus{border-color:#70cdef;}"
            "QComboBox:enabled:hover{border-color:#68635f;}"
            "QComboBox:disabled{color:#77736c;border-color:#2f2f2f;}"
            "QComboBox::drop-down{width:16px;border:0;}"
            "QComboBox::down-arrow{image:none;}"
            ));
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            hide();
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }
};

class ChatInput final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<void(const QImage &)> imagePasted;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Paste) && pasteImage()) {
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        std::unique_ptr<QMenu> menu(createStandardContextMenu());
        const auto *data = QApplication::clipboard()->mimeData();
        if (data && data->hasImage()) {
            auto *paste = menu->addAction(QStringLiteral("Paste image"));
            connect(paste, &QAction::triggered, this, [this] { pasteImage(); });
        }
        menu->exec(event->globalPos());
    }

private:
    bool pasteImage()
    {
        const auto *data = QApplication::clipboard()->mimeData();
        if (!imagePasted || !data || !data->hasImage())
            return false;
        imagePasted(QApplication::clipboard()->image());
        return true;
    }
};

QString linkMarkup(const QString &text)
{
    static const QRegularExpression links(
        R"rx(\[([^\]\n]+)\]\((https?://[^\s<>()]+)\)|(https?://[^\s<>()]+))rx");
    auto matches = links.globalMatch(text);
    if (!matches.hasNext())
        return {};

    QString html;
    qsizetype offset = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const bool markdown = !match.captured(2).isEmpty();
        QString url = match.captured(markdown ? 2 : 3);
        if (!markdown) {
            while (!url.isEmpty() && QStringLiteral(".,;:!?]").contains(url.back()))
                url.chop(1);
        }
        html += text.sliced(offset, match.capturedStart() - offset).toHtmlEscaped();
        const QString label = markdown ? match.captured(1) : url;
        html += QStringLiteral(R"(<a style="color:#70cdef" href="%1">%2</a>)")
                    .arg(url.toHtmlEscaped(), label.toHtmlEscaped());
        offset = markdown ? match.capturedEnd() : match.capturedStart() + url.size();
    }
    html += text.sliced(offset).toHtmlEscaped();
    return html.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

QLabel *imagePreview(const ChatImage &image, int side, QWidget *parent)
{
    auto *preview = new QLabel(parent);
    preview->setPixmap(image.thumbnail.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    preview->setAccessibleName(QStringLiteral("Attached image"));
    preview->setStyleSheet(QStringLiteral("background:transparent;"));
    return preview;
}

class MessageRow : public QWidget {
public:
    explicit MessageRow(bool user, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_user(user)
    {
        auto *outer = new QHBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(7);

        m_avatar = new QLabel(this);
        m_avatar->setFixedSize(16, 16);
        m_avatar->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_avatar->setStyleSheet(QStringLiteral("background:transparent;"));

        m_bubble = new QFrame(this);
        m_bubble->setAttribute(Qt::WA_StyledBackground, true);
        m_column = new QVBoxLayout(m_bubble);
        m_column->setSpacing(4);

        if (m_user) {
            m_column->setContentsMargins(10, 8, 10, 8);
            m_bubble->setObjectName(QStringLiteral("chatUserBubble"));
            m_bubble->setStyleSheet(
                QStringLiteral("QFrame#chatUserBubble{background:#35322c;border-radius:11px;}"));
            m_avatar->hide();
            outer->addStretch(1);
            outer->addWidget(m_bubble, 0);
        } else {
            m_column->setContentsMargins(0, 0, 0, 0);
            m_bubble->setStyleSheet(QStringLiteral("background:transparent;"));
            outer->addWidget(m_avatar, 0, Qt::AlignTop);
            outer->addWidget(m_bubble, 1);
        }
    }

    void setAvatar(const QPixmap &pixmap) { m_avatar->setPixmap(pixmap); }

    QLabel *setSpeaker(const QString &name)
    {
        auto *label = new QLabel(name, m_bubble);
        label->setTextFormat(Qt::PlainText);
        label->setStyleSheet(QStringLiteral(
            "color:#ff80a7;background:transparent;font-size:11px;font-weight:750;"));
        m_column->addWidget(label);
        return label;
    }

    QLabel *addBody(const QString &text, const QColor &color)
    {
        auto *label = new QLabel(m_bubble);
        const QString html = m_user ? QString() : linkMarkup(text);
        label->setTextFormat(html.isEmpty() ? Qt::PlainText : Qt::RichText);
        label->setText(html.isEmpty() ? text : html);
        label->setWordWrap(true);
        label->setOpenExternalLinks(!html.isEmpty());
        auto interaction = Qt::TextInteractionFlags(Qt::TextSelectableByMouse);
        if (!html.isEmpty())
            interaction |= Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard;
        label->setTextInteractionFlags(interaction);
        label->setStyleSheet(
            QStringLiteral("color:%1;background:transparent;font-size:14px;").arg(color.name()));
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        m_column->addWidget(label);
        return label;
    }

    void addActions(QWidget *actions) { m_column->addWidget(actions); }

    void addImages(const QList<ChatImage> &images)
    {
        auto *strip = new QHBoxLayout;
        strip->setSpacing(5);
        for (const auto &image : images)
            strip->addWidget(imagePreview(image, 36, m_bubble));
        strip->addStretch();
        m_column->addLayout(strip);
    }

    void setContentMaxWidth(int width)
    {
        if (width <= 0)
            return;
        const int maximum = m_user ? qMax(120, int(width * 0.86)) : qMax(120, width - 24);
        m_bubble->setMaximumWidth(maximum);
    }

private:
    bool m_user;
    QFrame *m_bubble;
    QVBoxLayout *m_column;
    QLabel *m_avatar;
};

} // namespace

struct ChatPane::Impl : public QObject {
    explicit Impl(ChatPane *owner)
        : QObject(nullptr)
        , q(owner)
    {
        const QSettings saved;
        for (Harness harness : kHarnesses) {
            const QString key = QStringLiteral("coWrite/") + harnessName(harness) + '/';
            m_settings.insert(int(harness), Settings{
                saved.value(key + QStringLiteral("model"), QStringLiteral("Default")).toString(),
                saved.value(key + QStringLiteral("effort"), QStringLiteral("Default")).toString()
            });
        }
        m_agents = std::make_unique<Agents>();
        buildUi();
        connectUi();
        updatePlaceholder();
        updatePendingUi();
        updateComposerEnabled();
        renderChat();
    }

    ~Impl() override
    {
        saveProviderSettings();
        m_alive = false;
        m_run = Run{};
        if (m_agents)
            m_agents->cancel();
    }

    void setNote(const QString &id, const QString &title, const QString &text)
    {
        if (id == m_noteId) {
            m_noteTitle = title;
            m_noteText = text;
            updateComposerEnabled();
            reportContext();
            return;
        }

        if (busy())
            cancelRun(false);
        m_noteId = id;
        m_noteTitle = title;
        m_noteText = text;
        if (!id.isEmpty())
            noteState(id);
        m_input->clear();
        resetExtras();
        updatePlaceholder();
        updateComposerEnabled();
        renderChat();
        reportContext();
    }

    void renameNote(const QString &oldId, const QString &newId, const QString &title)
    {
        const bool active = m_noteId == oldId;
        if (active) cancelRun(false);
        const auto position = m_noteOrder.indexOf(oldId);
        m_noteOrder.removeAll(newId);
        m_noteOrder.removeAll(oldId);
        m_notes.remove(newId);
        if (m_notes.contains(oldId)) {
            m_notes.insert(newId, m_notes.take(oldId));
            m_noteOrder.insert(qBound(qsizetype(0), position, m_noteOrder.size()), newId);
        }
        if (active) {
            m_noteId = newId;
            m_noteTitle = title;
            renderChat();
            reportContext();
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_scroll->viewport() && event->type() == QEvent::Resize) {
            relayoutRows();
        } else if (watched == m_providerBar && event->type() == QEvent::Resize) {
            if (m_providerBar->layout())
                m_providerBar->layout()->activate();
            updateProviderIndicator(false);
        } else if (watched == m_input) {
            if (event->type() == QEvent::FocusIn)
                setComposerFocus(true);
            else if (event->type() == QEvent::FocusOut)
                setComposerFocus(false);
        } else if (watched == m_popup && event->type() == QEvent::Hide) {
            m_gear->setChecked(false);
            saveProviderSettings();
        }
        return QObject::eventFilter(watched, event);
    }

    ChatPane *q = nullptr;
    std::unique_ptr<Agents> m_agents;
    bool m_alive = true;

    QString m_noteId;
    QString m_noteTitle;
    QString m_noteText;
    Mode m_mode = Mode::Solo;
    Harness m_provider = Harness::Codex;
    QHash<QString, NoteState> m_notes;
    QList<QString> m_noteOrder;
    QHash<int, Settings> m_settings;
    Run m_run;
    quint64 m_cycle = 0;
    quint64 m_nextMessageId = 1;

    QScrollArea *m_scroll = nullptr;
    QWidget *m_container = nullptr;
    QVBoxLayout *m_chatLayout = nullptr;
    QVector<MessageRow *> m_rows;
    QVector<QWidget *> m_rendered;

    ChatInput *m_input = nullptr;
    QToolButton *m_web = nullptr;
    QWidget *m_replyBanner = nullptr;
    QLabel *m_replyLabel = nullptr;
    QWidget *m_imageTray = nullptr;
    QHBoxLayout *m_imageLayout = nullptr;
    QLabel *m_draftNotice = nullptr;
    ReplyQuote m_reply;
    QList<ChatImage> m_images;
    QToolButton *m_send = nullptr;
    QToolButton *m_soloButton = nullptr;
    QToolButton *m_councilButton = nullptr;
    QAbstractButton *m_gear = nullptr;
    QFrame *m_composerFrame = nullptr;
    QVector<ProviderButton *> m_providerButtons;
    QWidget *m_providerBar = nullptr;
    QWidget *m_providerIndicator = nullptr;
    QPropertyAnimation *m_indicatorAnimation = nullptr;

    SettingsPopup *m_popup = nullptr;
    QLabel *m_popupTitle = nullptr;
    QComboBox *m_modelCombo = nullptr;
    QComboBox *m_effortCombo = nullptr;
    QLabel *m_catalogStatus = nullptr;
    QToolButton *m_refreshModels = nullptr;
    ModelCatalog m_catalog;
    ModelList m_visibleCatalog;
    bool m_settingsEdited = false;

    static Conversation &conversation(NoteState &state, Mode mode)
    {
        return mode == Mode::Solo ? state.solo : state.council;
    }

    static const Conversation &conversation(const NoteState &state, Mode mode)
    {
        return mode == Mode::Solo ? state.solo : state.council;
    }

    bool busy() const { return m_run.phase != Phase::Idle; }

    NoteState &noteState(const QString &id)
    {
        auto it = m_notes.find(id);
        if (it == m_notes.end()) {
            m_notes.insert(id, NoteState{});
            m_noteOrder.append(id);
            while (m_noteOrder.size() > kMaxNotes) {
                const QString oldest = m_noteOrder.takeFirst();
                if (oldest != m_noteId && oldest != m_run.noteId)
                    m_notes.remove(oldest);
            }
            it = m_notes.find(id);
        } else {
            m_noteOrder.removeAll(id);
            m_noteOrder.append(id);
        }
        return *it;
    }

    Message *messageById(const QString &noteId, Mode mode, quint64 id)
    {
        auto it = m_notes.find(noteId);
        if (it == m_notes.end())
            return nullptr;
        Conversation &conv = conversation(*it, mode);
        for (Message &message : conv.messages) {
            if (message.id == id)
                return &message;
        }
        return nullptr;
    }

    void buildUi()
    {
        auto *root = new QVBoxLayout(q);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        auto *modeBar = new QWidget(q);
        auto *modeLayout = new QHBoxLayout(modeBar);
        modeLayout->setContentsMargins(10, 9, 10, 3);
        modeLayout->setSpacing(4);

        const QString toggleBase = QStringLiteral(
            "QToolButton{background:transparent;border:0;border-radius:12px;padding:4px 9px;"
            "color:#c0bbb3;font-size:12px;font-weight:650;}"
            "QToolButton:enabled:hover{background:#383532;}");

        m_soloButton = new QToolButton(modeBar);
        m_soloButton->setObjectName(QStringLiteral("chatModeSolo"));
        m_soloButton->setText(QStringLiteral("Co-write"));
        m_soloButton->setCheckable(true);
        m_soloButton->setCursor(Qt::PointingHandCursor);
        m_soloButton->setStyleSheet(
            toggleBase + QStringLiteral("QToolButton:checked{background:#2b3932;color:#76e3b4;}"
                                        "QToolButton:checked:enabled:hover{background:#394d42;}"));

        m_councilButton = new QToolButton(modeBar);
        m_councilButton->setObjectName(QStringLiteral("chatModeCouncil"));
        m_councilButton->setText(QStringLiteral("Council"));
        m_councilButton->setCheckable(true);
        m_councilButton->setCursor(Qt::PointingHandCursor);
        m_councilButton->setStyleSheet(
            toggleBase + QStringLiteral("QToolButton:checked{background:#3b2c36;color:#ff80a7;}"
                                        "QToolButton:checked:enabled:hover{background:#503a48;}"));

        auto *modeGroup = new QButtonGroup(this);
        modeGroup->setExclusive(true);
        modeGroup->addButton(m_soloButton);
        modeGroup->addButton(m_councilButton);
        m_soloButton->setChecked(true);

        modeLayout->addWidget(m_soloButton);
        modeLayout->addWidget(m_councilButton);
        modeLayout->addStretch(1);
        root->addWidget(modeBar);

        m_scroll = new QScrollArea(q);
        m_scroll->setWidgetResizable(true);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_scroll->setStyleSheet(QStringLiteral(
            "QScrollArea{background:transparent;border:0;}"
            "QScrollBar:vertical{background:transparent;width:8px;margin:2px;}"
            "QScrollBar::handle:vertical{background:#3d3b38;border-radius:4px;min-height:24px;}"
            "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
            "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"));

        m_container = new QWidget;
        m_container->setStyleSheet(QStringLiteral("background:transparent;"));
        m_chatLayout = new QVBoxLayout(m_container);
        m_chatLayout->setContentsMargins(11, 14, 11, 12);
        m_chatLayout->setSpacing(13);
        m_chatLayout->addStretch(1);
        m_scroll->setWidget(m_container);
        m_scroll->viewport()->installEventFilter(this);
        root->addWidget(m_scroll, 1);

        auto *divider = new QFrame(q);
        divider->setFixedHeight(1);
        divider->setStyleSheet(QStringLiteral("background:#353535;border:0;"));
        root->addWidget(divider);

        auto *composer = new QWidget(q);
        auto *composerLayout = new QVBoxLayout(composer);
        composerLayout->setContentsMargins(11, 8, 11, 10);
        composerLayout->setSpacing(6);

        auto *providerBar = new QWidget(composer);
        m_providerBar = providerBar;
        providerBar->installEventFilter(this);
        auto *providerLayout = new QHBoxLayout(providerBar);
        providerLayout->setContentsMargins(3, 0, 0, 0);
        providerLayout->setSpacing(3);

        auto *providerGroup = new QButtonGroup(this);
        providerGroup->setExclusive(true);
        for (Harness harness : kHarnesses) {
            auto *button = new ProviderButton(harness, providerBar);
            button->setObjectName(QStringLiteral("chatProvider") + QString::number(int(harness)));
            m_providerButtons.append(button);
            providerGroup->addButton(button, int(harness));
            connect(button, &QAbstractButton::clicked, this,
                    [this, harness] { selectProvider(harness); });
            providerLayout->addWidget(button);
        }
        providerLayout->addStretch(1);

        m_gear = new SliderButton(providerBar);
        m_gear->setObjectName(QStringLiteral("chatGear"));
        providerLayout->addWidget(m_gear);

        m_providerIndicator = new QWidget(providerBar);
        m_providerIndicator->setObjectName(QStringLiteral("chatProviderIndicator"));
        m_providerIndicator->setFixedSize(11, 2);
        m_providerIndicator->setAttribute(Qt::WA_StyledBackground, true);
        m_providerIndicator->setStyleSheet(
            QStringLiteral("background:%1;border-radius:1px;").arg(kGreen.name()));
        m_providerIndicator->raise();
        m_indicatorAnimation = new QPropertyAnimation(m_providerIndicator, "pos", this);
        m_indicatorAnimation->setDuration(270);
        QEasingCurve indicatorCurve(QEasingCurve::BezierSpline);
        indicatorCurve.addCubicBezierSegment(QPointF(0.45, 0.0), QPointF(0.25, 1.0),
                                             QPointF(1.0, 1.0));
        m_indicatorAnimation->setEasingCurve(indicatorCurve);

        composerLayout->addWidget(providerBar);

        m_composerFrame = new QFrame(composer);
        m_composerFrame->setObjectName(QStringLiteral("chatComposer"));
        m_composerFrame->setAttribute(Qt::WA_StyledBackground, true);
        m_composerFrame->setMinimumHeight(38);
        auto *inputLayout = new QHBoxLayout(m_composerFrame);
        inputLayout->setContentsMargins(13, 4, 5, 4);
        inputLayout->setSpacing(4);

        m_input = new ChatInput(m_composerFrame);
        m_input->setObjectName(QStringLiteral("chatPrompt"));
        m_input->setMaxLength(kMaxInputChars);
        m_input->setAccessibleName(QStringLiteral("Message your co-writer"));
        m_input->setStyleSheet(QStringLiteral(
            "QLineEdit{background:transparent;border:0;color:#f3e9d5;font-size:13px;"
            "padding:5px 0;selection-background-color:#70cdef;selection-color:#14232a;}"));
        m_input->installEventFilter(this);

        m_send = new QToolButton(m_composerFrame);
        m_send->setObjectName(QStringLiteral("chatSend"));
        m_send->setFixedSize(27, 27);
        m_send->setCursor(Qt::PointingHandCursor);
        m_send->setAccessibleName(QStringLiteral("Send message"));

        inputLayout->addWidget(m_input, 1);
        inputLayout->addWidget(m_send, 0, Qt::AlignVCenter);
        m_replyBanner = new QWidget(composer);
        auto *replyLayout = new QHBoxLayout(m_replyBanner);
        replyLayout->setContentsMargins(6, 0, 3, 0);
        replyLayout->setSpacing(4);
        m_replyLabel = new QLabel(m_replyBanner);
        m_replyLabel->setTextFormat(Qt::PlainText);
        m_replyLabel->setWordWrap(true);
        m_replyLabel->setStyleSheet(QStringLiteral("color:#c0bbb3;font-size:11px;"));
        auto *clearReply = dismissButton(QStringLiteral("Cancel reply"));
        replyLayout->addWidget(m_replyLabel, 1);
        replyLayout->addWidget(clearReply);
        connect(clearReply, &QToolButton::clicked, this, [this] {
            m_reply = {};
            updateReplyBanner();
            m_input->setFocus();
        });
        composerLayout->addWidget(m_replyBanner);
        m_replyBanner->hide();

        m_imageTray = new QWidget(composer);
        m_imageLayout = new QHBoxLayout(m_imageTray);
        m_imageLayout->setContentsMargins(3, 0, 3, 0);
        m_imageLayout->setSpacing(5);
        composerLayout->addWidget(m_imageTray);
        m_imageTray->hide();

        m_draftNotice = new QLabel(composer);
        m_draftNotice->setWordWrap(true);
        m_draftNotice->setTextFormat(Qt::PlainText);
        m_draftNotice->setStyleSheet(QStringLiteral("color:#ff80a7;font-size:11px;"));
        composerLayout->addWidget(m_draftNotice);
        m_draftNotice->hide();

        m_web = new QToolButton(m_composerFrame);
        m_web->setObjectName(QStringLiteral("chatWebSearch"));
        m_web->setText(QStringLiteral("Web"));
        m_web->setAccessibleName(QStringLiteral("Web search"));
        m_web->setCursor(Qt::PointingHandCursor);
        m_web->setCheckable(true);
        m_web->setStyleSheet(QStringLiteral(
            "QToolButton{color:#c0bbb3;background:transparent;border:0;padding:4px 5px;font-size:11px;}"
            "QToolButton:enabled:hover{background:#383532;}"
            "QToolButton:checked{color:#a5e7fc;background:#263740;}"
            "QToolButton:checked:enabled:hover{background:#344952;}"
            "QToolButton:disabled{color:#77736c;}"));
        inputLayout->insertWidget(1, m_web);
        m_input->imagePasted = [this](const QImage &image) { attachImage(image); };

        composerLayout->addWidget(m_composerFrame);
        root->addWidget(composer);

        setComposerFocus(false);

        m_popup = new SettingsPopup(q);
        m_popup->installEventFilter(this);
        auto *popupLayout = new QVBoxLayout(m_popup);
        popupLayout->setContentsMargins(10, 10, 10, 10);
        popupLayout->setSpacing(5);

        m_popupTitle = new QLabel(m_popup);
        m_popupTitle->setObjectName(QStringLiteral("chatSettingsTitle"));
        popupLayout->addWidget(m_popupTitle);

        auto *modelRow = new QHBoxLayout;
        auto *modelLabel = new QLabel(QStringLiteral("Model"), m_popup);
        modelRow->addWidget(modelLabel, 1);
        m_refreshModels = new QToolButton(m_popup);
        m_refreshModels->setText(QStringLiteral("↻"));
        m_refreshModels->setToolTip(QStringLiteral("Refresh models"));
        m_refreshModels->setAccessibleName(QStringLiteral("Refresh models"));
        modelRow->addWidget(m_refreshModels);
        popupLayout->addLayout(modelRow);
        m_modelCombo = new CompactCombo(m_popup);
        m_modelCombo->setObjectName(QStringLiteral("chatModel"));
        m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        modelLabel->setBuddy(m_modelCombo);
        popupLayout->addWidget(m_modelCombo);
        m_catalogStatus = new QLabel(m_popup);
        m_catalogStatus->setObjectName(QStringLiteral("catalogStatus"));
        m_catalogStatus->setWordWrap(true);
        m_catalogStatus->setStyleSheet(QStringLiteral("color:#a49f96;font-size:11px;"));
        popupLayout->addWidget(m_catalogStatus);
        m_catalogStatus->hide();

        auto *effortLabel = new QLabel(QStringLiteral("Effort"), m_popup);
        popupLayout->addWidget(effortLabel);
        m_effortCombo = new CompactCombo(m_popup);
        m_effortCombo->setObjectName(QStringLiteral("chatEffort"));
        m_effortCombo->setMaxVisibleItems(8);
        popupLayout->addWidget(m_effortCombo);

        m_popup->setFixedWidth(190);
        m_popup->hide();

        selectProvider(Harness::Codex, false);
        QTimer::singleShot(0, this, [this] { updateProviderIndicator(false); });
    }

    void connectUi()
    {
        connect(m_input, &QLineEdit::returnPressed, this, [this] { submitPrompt(); });
        connect(m_send, &QToolButton::clicked, this, [this] {
            if (busy())
                cancelRun(true);
            else
                submitPrompt();
        });
        connect(m_soloButton, &QToolButton::clicked, this, [this] { setMode(Mode::Solo); });
        connect(m_councilButton, &QToolButton::clicked, this, [this] { setMode(Mode::Council); });
        connect(m_gear, &QAbstractButton::clicked, this, [this] {
            if (m_popup->isVisible())
                m_popup->hide();
            else
                showSettings();
        });
        connect(m_refreshModels, &QToolButton::clicked, this, [this] { loadModels(true); });
        connect(m_modelCombo, &QComboBox::activated, this, [this](int index) {
            if (index < 0) return;
            m_settings[int(m_provider)].model = m_modelCombo->itemData(index).toString();
            m_settingsEdited = true;
            syncEffortAvailability();
            updateGearTooltip();
        });
        connect(m_effortCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
            m_settings[int(m_provider)].effort =
                text.isEmpty() ? QStringLiteral("Default") : text;
            m_settingsEdited = true;
            updateGearTooltip();
        });
        updateGearTooltip();
    }

    void setComposerFocus(bool focused)
    {
        m_composerFrame->setStyleSheet(
            QStringLiteral("QFrame#chatComposer{background:#252525;border:1px solid %1;"
                           "border-radius:19px;}")
                .arg(focused ? QStringLiteral("#70cdef") : QStringLiteral("#353535")));
    }

    void selectProvider(Harness harness, bool animate = true)
    {
        const bool changed = harness != m_provider;
        m_provider = harness;
        if (changed && q->providerUsageChanged)
            q->providerUsageChanged(harness);
        for (ProviderButton *button : m_providerButtons)
            button->setChecked(button->harness() == harness);
        updateProviderIndicator(animate);
        updateGearTooltip();
        updateComposerEnabled();
        if (m_popup->isVisible())
            showSettings();
    }

    void updateProviderIndicator(bool animate)
    {
        if (!m_providerBar || !m_providerIndicator)
            return;
        ProviderButton *selected = nullptr;
        for (ProviderButton *button : m_providerButtons) {
            if (button->harness() == m_provider) {
                selected = button;
                break;
            }
        }
        if (!selected || selected->width() <= 0 || m_providerBar->height() <= 0)
            return;

        const QPoint target(selected->x()
                                + (selected->width() - m_providerIndicator->width()) / 2,
                            m_providerBar->height() - m_providerIndicator->height());
        if (!animate || !m_providerBar->isVisible() || m_providerIndicator->pos() == target) {
            if (m_indicatorAnimation)
                m_indicatorAnimation->stop();
            m_providerIndicator->move(target);
            return;
        }
        m_indicatorAnimation->stop();
        m_indicatorAnimation->setStartValue(m_providerIndicator->pos());
        m_indicatorAnimation->setEndValue(target);
        m_indicatorAnimation->start();
    }

    void syncEffortAvailability()
    {
        auto &settings = m_settings[int(m_provider)];
        QStringList levels = m_visibleCatalog.defaultEfforts;
        const auto id = m_modelCombo->currentData().toString();
        for (const auto &model : m_visibleCatalog.models) {
            if (model.id == id) { levels = model.efforts; break; }
        }

        const QSignalBlocker block(m_effortCombo);
        m_effortCombo->clear();
        m_effortCombo->addItem(QStringLiteral("Default"));
        for (auto &level : levels) {
            if (!level.isEmpty()) level[0] = level[0].toUpper();
        }
        m_effortCombo->addItems(levels);
        const int choice = m_effortCombo->findText(settings.effort, Qt::MatchFixedString);
        m_effortCombo->setCurrentIndex(qMax(0,choice));
        m_effortCombo->setEnabled(!levels.isEmpty());
        if (choice < 0 && m_visibleCatalog.error.isEmpty() && !m_visibleCatalog.models.isEmpty()) {
            settings.effort = QStringLiteral("Default");
            m_settingsEdited = true;
        }
    }

    void populateModels()
    {
        const QSignalBlocker blockModel(m_modelCombo);
        m_modelCombo->hidePopup();
        m_modelCombo->clear();
        m_modelCombo->addItem(QStringLiteral("Default"), QStringLiteral("Default"));
        for (const auto &model : m_visibleCatalog.models)
            m_modelCombo->addItem(model.label, model.id);

        auto &settings = m_settings[int(m_provider)];
        const int choice = m_modelCombo->findData(settings.model);
        m_modelCombo->setCurrentIndex(qMax(0,choice));
        if (choice < 0 && m_visibleCatalog.error.isEmpty() && !m_visibleCatalog.models.isEmpty()) {
            settings.model = QStringLiteral("Default");
            settings.effort = QStringLiteral("Default");
            m_settingsEdited = true;
        }
        syncEffortAvailability();
        updateGearTooltip();
    }

    void loadModels(bool force = false)
    {
        const Harness requested = m_provider;
        m_refreshModels->setEnabled(false);
        m_catalogStatus->setText(QStringLiteral("Loading models…"));
        m_catalogStatus->show();
        if (m_popup->isVisible()) positionSettings();
        m_catalog.refresh(requested, [this,requested](const ModelList &result) {
            if (requested != m_provider) return;
            m_visibleCatalog = result;
            populateModels();
            m_catalogStatus->setText(result.error);
            m_catalogStatus->setVisible(!result.error.isEmpty());
            m_refreshModels->setEnabled(true);
            if (m_popup->isVisible()) positionSettings();
        },force);
    }

    void setMode(Mode mode)
    {
        if (m_mode == mode)
            return;
        if (busy())
            cancelRun(false);
        m_mode = mode;
        m_reply = {};
        updateReplyBanner();
        updateComposerEnabled();
        m_soloButton->setChecked(mode == Mode::Solo);
        m_councilButton->setChecked(mode == Mode::Council);
        updatePlaceholder();
        renderChat();
        reportContext();
    }

    void showSettings()
    {
        m_popupTitle->setText(harnessName(m_provider));
        m_modelCombo->setAccessibleName(harnessName(m_provider) + QStringLiteral(" model"));
        m_effortCombo->setAccessibleName(harnessName(m_provider) + QStringLiteral(" effort"));
        const bool opening = !m_popup->isVisible();
        m_visibleCatalog = {};
        populateModels();
        loadModels();
        positionSettings();
        m_popup->show();
        m_popup->raise();
        if (opening) fadeIn(m_popup);
        m_gear->setChecked(true);
    }

    void positionSettings()
    {
        m_popup->adjustSize();

        const QPoint anchor = m_gear->mapToGlobal(QPoint(m_gear->width(), 0));
        int x = anchor.x() - m_popup->width();
        int y = anchor.y() - m_popup->height() - 7;
        if (QScreen *screen = QGuiApplication::screenAt(anchor)) {
            const QRect available = screen->availableGeometry();
            x = qBound(available.left() + 4, x, available.right() - m_popup->width() - 4);
            if (y < available.top() + 4)
                y = m_gear->mapToGlobal(QPoint(0, m_gear->height() + 7)).y();
            y = qBound(available.top() + 4, y, available.bottom() - m_popup->height() - 4);
        }
        m_popup->move(x, y);
    }

    void updateGearTooltip()
    {
        const Settings settings = m_settings.value(int(m_provider));
        QStringList parts{harnessName(m_provider)};
        if (settings.model != QStringLiteral("Default"))
            parts << settings.model;
        if (settings.effort != QStringLiteral("Default"))
            parts << settings.effort;
        m_gear->setToolTip(parts.join(QStringLiteral(" · "))
                           + QStringLiteral(" — model and effort"));
    }

    void saveProviderSettings()
    {
        if (!m_settingsEdited) return;
        QSettings saved;
        for (Harness harness : kHarnesses) {
            const QString key = QStringLiteral("coWrite/") + harnessName(harness) + '/';
            const auto settings = m_settings.value(int(harness));
            saved.setValue(key + QStringLiteral("model"), settings.model);
            saved.setValue(key + QStringLiteral("effort"), settings.effort);
        }
        m_settingsEdited = false;
    }

    void updatePlaceholder()
    {
        if (m_noteId.isEmpty())
            m_input->setPlaceholderText(QStringLiteral("Open a note to chat"));
        else
            m_input->setPlaceholderText(m_mode == Mode::Solo ? QStringLiteral("Ask Co-write…")
                                                             : QStringLiteral("Ask Council…"));
        m_input->setToolTip(QStringLiteral("Paste an image here to attach it. Images require a vision-capable model."));
    }

    static QToolButton *dismissButton(const QString &label)
    {
        auto *button = makeActionButton(QStringLiteral("×"));
        button->setFixedSize(19, 21);
        button->setAccessibleName(label);
        button->setToolTip(label);
        button->setStyleSheet(QStringLiteral(
            "QToolButton{font-size:17px;color:#f3e9d5;border:0;background:transparent;}"
            "QToolButton:hover{background:#353535;border-radius:5px;}"));
        return button;
    }

    void updateReplyBanner()
    {
        m_replyBanner->setVisible(m_reply.id != 0);
        m_replyLabel->setText(QStringLiteral("Reply to %1 · %2")
            .arg(m_reply.speaker, bounded(normalizedText(m_reply.text), 120)));
    }

    void updateImageTray()
    {
        while (auto *item = m_imageLayout->takeAt(0)) {
            if (auto *widget = item->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }
        for (qsizetype index = 0; index < m_images.size(); ++index) {
            auto *chip = new QWidget(m_imageTray);
            auto *layout = new QHBoxLayout(chip);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);
            layout->addWidget(imagePreview(m_images.at(index), 30, chip));
            auto *remove = dismissButton(QStringLiteral("Remove image %1").arg(index + 1));
            layout->addWidget(remove);
            connect(remove, &QToolButton::clicked, this, [this, index] {
                m_images.removeAt(index);
                m_draftNotice->hide();
                updateImageTray();
                m_input->setFocus();
            });
            m_imageLayout->addWidget(chip);
        }
        m_imageLayout->addStretch();
        m_imageTray->setVisible(!m_images.isEmpty());
    }

    void attachImage(const QImage &image)
    {
        if (m_noteId.isEmpty())
            return;
        if (m_images.size() >= kMaxImages) {
            m_draftNotice->setText(QStringLiteral("Up to %1 images per message.").arg(kMaxImages));
            m_draftNotice->show();
            return;
        }
        QImage scaled = image;
        if (image.width() > kImageSide || image.height() > kImageSide)
            scaled = image.scaled(kImageSide, kImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto file = std::make_shared<QTemporaryFile>(QDir::temp().filePath("sava-chat-XXXXXX.jpg"));
        if (scaled.isNull() || !file->open() || !scaled.save(file.get(), "JPEG", 88)
            || !file->flush() || file->size() > kMaxImageBytes) {
            m_draftNotice->setText(QStringLiteral("Couldn’t attach this image."));
            m_draftNotice->show();
            return;
        }
        file->close();
        m_images.append({file, QPixmap::fromImage(
            scaled.scaled(84, 84, Qt::KeepAspectRatio, Qt::SmoothTransformation))});
        m_draftNotice->hide();
        updateImageTray();
    }

    void resetExtras()
    {
        m_reply = {};
        m_images.clear();
        m_web->setChecked(false);
        m_draftNotice->hide();
        updateReplyBanner();
        updateImageTray();
    }

    void updateComposerEnabled()
    {
        m_input->setEnabled(!m_noteId.isEmpty());
        m_send->setEnabled(busy() || !m_noteId.isEmpty());
        const bool supported = Agents::supportsWebSearch(m_provider);
        m_web->setEnabled(supported && !m_noteId.isEmpty());
        if (!supported) m_web->setChecked(false);
        QString webTip = !supported ? QStringLiteral("Pi has no built-in web search.")
            : m_mode == Mode::Council
                ? QStringLiteral("Search the web with %1, the first Council provider.").arg(harnessName(m_provider))
                : QStringLiteral("Search the web with %1 for this message.").arg(harnessName(m_provider));
        if (m_provider == Harness::OpenCode)
            webTip += QStringLiteral("\nUses OpenCode’s configured search integration.");
        m_web->setToolTip(webTip);
    }

    void updatePendingUi()
    {
        const bool running = busy();
        m_send->setText(running ? QStringLiteral("■") : QStringLiteral("↑"));
        m_send->setToolTip(running ? QStringLiteral("Stop") : QStringLiteral("Send message"));
        m_send->setAccessibleName(running ? QStringLiteral("Stop") : QStringLiteral("Send message"));
        const QString background = running ? kPink.name() : kBlue.name();
        const QString hover = running ? QStringLiteral("#ffa1bf") : QStringLiteral("#a5e7fc");
        m_send->setStyleSheet(
            QStringLiteral("QToolButton{background:%1;border:0;border-radius:13px;"
                           "color:#14232a;font-size:15px;font-weight:700;}"
                           "QToolButton:hover{background:%2;}"
                           "QToolButton:disabled{background:#353535;color:#77736c;}")
                .arg(background, hover));
        updateComposerEnabled();
    }

    void submitPrompt()
    {
        if (busy() || m_noteId.isEmpty())
            return;
        const QString text = m_input->text().trimmed();
        if (text.isEmpty() && m_images.isEmpty())
            return;

        NoteState &state = noteState(m_noteId);
        Conversation &conv = conversation(state, m_mode);
        Message message;
        message.id = m_nextMessageId++;
        message.user = true;
        message.text = bounded(text, kMaxInputChars);
        message.quote = m_reply;
        message.images = m_images;
        QList<ChatImage> requestImages = m_images;
        if (m_reply.id && requestImages.isEmpty()) {
            if (const auto *target = messageById(m_noteId, m_mode, m_reply.id))
                requestImages = target->images;
        }
        conv.messages.append(message);
        trimConversation(conv);
        m_input->clear();

        Run run;
        run.phase = m_mode == Mode::Solo ? Phase::Solo : Phase::Council;
        run.mode = m_mode;
        run.noteId = m_noteId;
        run.cycle = ++m_cycle;
        run.harness = m_provider;
        run.webSearch = m_web->isChecked();
        run.images = requestImages;
        resetExtras();
        if (run.phase == Phase::Council) {
            run.order.append(m_provider);
            for (Harness harness : kHarnesses) {
                if (harness != m_provider)
                    run.order.append(harness);
            }
        }
        m_run = run;
        updatePendingUi();
        renderChat();
        dispatchRun();
        reportContext();
    }

    // Single place where the run lifecycle advances: the current turn is sent,
    // and a reply either starts the next council turn or ends the run.
    void dispatchRun()
    {
        if (m_run.phase == Phase::Idle)
            return;

        Harness harness = m_run.harness;
        if (m_run.phase == Phase::Council) {
            if (m_run.position < 0 || m_run.position >= m_run.order.size()) {
                finishRun();
                renderChat();
                return;
            }
            harness = m_run.order.at(m_run.position);
        }

        const Settings settings = m_settings.value(int(harness));
        AgentRequest request;
        request.harness = harness;
        if (isConcreteModel(settings.model))
            request.model = settings.model.trimmed();
        const bool openCodeBlocked =
            harness == Harness::OpenCode
            && (!isConcreteModel(settings.model) || settings.model.contains(QLatin1Char('#')));
        if (settings.effort != QStringLiteral("Default") && !openCodeBlocked)
            request.effort = settings.effort.toLower();
        request.prompt = composePrompt(m_run.mode, harness);
        if (m_run.position == 0) {
            request.webSearch = m_run.webSearch;
            for (const auto &image : m_run.images)
                request.images.append(QDir::toNativeSeparators(image.file->fileName()));
        }

        const quint64 cycle = m_run.cycle;
        const QString noteId = m_run.noteId;
        const bool queued = m_agents->start(
            request, [this, cycle, noteId, harness](AgentReply reply) {
                handleReply(reply, harness, cycle, noteId);
            });
        if (!queued) {
            failActive(QStringLiteral("%1 is not available right now.").arg(harnessName(harness)));
        }
    }

    void handleReply(const AgentReply &reply, Harness harness, quint64 cycle,
                     const QString &noteId)
    {
        if (!m_alive || m_run.phase == Phase::Idle || m_run.cycle != cycle
            || m_run.noteId != noteId) {
            return;
        }
        if (m_run.phase == Phase::Solo && m_run.harness != harness)
            return;
        if (m_run.phase == Phase::Council
            && (m_run.position >= m_run.order.size()
                || m_run.order.at(m_run.position) != harness)) {
            return;
        }

        auto it = m_notes.find(noteId);
        if (it == m_notes.end()) {
            finishRun();
            renderChat();
            return;
        }
        NoteState &state = *it;
        Conversation &conv = conversation(state, m_run.mode);

        if (reply.contextLimit > 0)
            state.contextLimit = reply.contextLimit;
        if (harness == m_provider && q->providerUsageChanged)
            q->providerUsageChanged(harness);

        if (!reply.ok) {
            Message error;
            error.id = m_nextMessageId++;
            error.notice = true;
            error.error = true;
            error.text = reply.error.isEmpty()
                ? QStringLiteral("The agent could not respond.")
                : bounded(reply.error, 600);
            conv.messages.append(error);
            trimConversation(conv);
            finishRun();
            renderChat();
            reportContext();
            return;
        }

        Message message;
        message.id = m_nextMessageId++;
        message.text = reply.text.trimmed().isEmpty()
            ? QStringLiteral("(No reply.)")
            : bounded(reply.text.trimmed(), 8000);
        message.harness = harness;
        message.model = m_settings.value(int(harness)).model;
        message.effort = m_settings.value(int(harness)).effort;
        deriveActions(message, m_noteText);
        conv.messages.append(message);
        trimConversation(conv);

        if (m_run.phase == Phase::Council && m_run.position + 1 < m_run.order.size()) {
            ++m_run.position;
            renderChat();
            reportContext();
            dispatchRun();
            return;
        }

        finishRun();
        renderChat();
        reportContext();
    }

    void failActive(const QString &error)
    {
        if (!busy())
            return;
        const QString noteId = m_run.noteId;
        const Mode mode = m_run.mode;
        auto it = m_notes.find(noteId);
        if (it != m_notes.end()) {
            Conversation &conv = conversation(*it, mode);
            Message message;
            message.id = m_nextMessageId++;
            message.notice = true;
            message.error = true;
            message.text = error;
            conv.messages.append(message);
            trimConversation(conv);
        }
        finishRun();
        renderChat();
        reportContext();
    }

    void finishRun()
    {
        m_run = Run{};
        updatePendingUi();
    }

    void cancelRun(bool refresh)
    {
        if (!busy())
            return;
        m_run = Run{};
        ++m_cycle;
        m_agents->cancel();
        updatePendingUi();
        if (refresh) {
            renderChat();
            reportContext();
        }
    }

    QString composePrompt(Mode mode, Harness harness) const
    {
        QString prompt = QStringLiteral("Give brief, specific writing help in plain text. "
                                        "Put paste-ready sentences in double quotes.");
        if (mode == Mode::Council) {
            prompt += QStringLiteral(" As %1 in the Council, build on or challenge earlier "
                                     "replies without repeating them.").arg(harnessName(harness));
        }
        if (!m_noteTitle.isEmpty())
            prompt += QStringLiteral("\n\nNote: ") + bounded(m_noteTitle, 200);
        if (!m_noteText.trimmed().isEmpty())
            prompt += QLatin1Char('\n') + bounded(m_noteText, kMaxNoteChars);
        prompt += QLatin1Char('\n');

        const NoteState *state = nullptr;
        auto it = m_notes.constFind(m_noteId);
        if (it != m_notes.constEnd())
            state = &*it;
        const Conversation *conv = state ? &conversation(*state, mode) : nullptr;

        int userIndex = -1;
        if (conv) {
            for (int index = conv->messages.size() - 1; index >= 0; --index) {
                if (conv->messages.at(index).user) {
                    userIndex = index;
                    break;
                }
            }
        }

        const Message *user = userIndex >= 0 ? &conv->messages.at(userIndex) : nullptr;
        const QString userText = user ? user->text : QString();

        QStringList history;
        int historyChars = 0;
        if (conv && userIndex > 0) {
            for (int index = userIndex - 1; index >= 0 && history.size() < kMaxHistoryMessages; --index) {
                const Message &message = conv->messages.at(index);
                if (message.notice || (user && user->quote.id == message.id))
                    continue;
                const QString line =
                    speakerName(message, mode) + QStringLiteral(": ") + bounded(messageText(message), 1200);
                if (historyChars + line.size() > kMaxHistoryChars)
                    break;
                historyChars += line.size();
                history.prepend(line);
            }
        }
        if (!history.isEmpty())
            prompt += QStringLiteral("\nConversation before this request:\n")
                + history.join(QLatin1Char('\n')) + QLatin1Char('\n');

        if (user && user->quote.id) {
            prompt += QStringLiteral("\nThe writer is replying to this earlier message from %1:\n%2\n")
                .arg(user->quote.speaker, user->quote.text);
        }
        if (!m_run.images.isEmpty()) {
            prompt += m_run.position == 0
                ? QStringLiteral("\nImages accompany the writer’s request. Say if you cannot view them.\n")
                : QStringLiteral("\nThe first Council agent received images. Refer to its response for visual details.\n");
        }
        prompt += QStringLiteral("\nWriter: ") + (userText.isEmpty()
            ? QStringLiteral("Please respond to the attached images.") : userText) + QLatin1Char('\n');

        if (mode == Mode::Council) {
            QStringList prior;
            if (conv && userIndex >= 0) {
                for (int index = userIndex + 1; index < conv->messages.size(); ++index) {
                    const Message &message = conv->messages.at(index);
                    if (message.notice)
                        continue;
                    prior.append(harnessName(message.harness) + QStringLiteral(": ")
                                 + bounded(message.text, 1000));
                }
            }
            if (!prior.isEmpty())
                prompt += QStringLiteral("\nThis round so far:\n")
                    + prior.join(QLatin1Char('\n')) + QLatin1Char('\n');
        }

        return prompt;
    }

    static QString messageText(const Message &message)
    {
        if (message.images.isEmpty())
            return message.text;
        return message.text.isEmpty() ? QStringLiteral("[Image attachment]")
                                      : message.text + QStringLiteral(" [Image attachment]");
    }

    static QString speakerName(const Message &message, Mode mode)
    {
        if (message.user)
            return QStringLiteral("Writer");
        return mode == Mode::Solo ? QStringLiteral("Co-write") : harnessName(message.harness);
    }

    static void trimConversation(Conversation &conv)
    {
        while (conv.messages.size() > kMaxMessages)
            conv.messages.removeFirst();
    }

    void reportContext()
    {
        if (!q->contextUpdated)
            return;
        const auto it = m_notes.constFind(m_noteId);
        if (it == m_notes.constEnd()) {
            q->contextUpdated(-1, -1);
            return;
        }
        qint64 estimated = estimateTokens(m_noteTitle) + estimateTokens(m_noteText) + 160;
        for (const Message &message : conversation(*it, m_mode).messages)
            estimated += estimateTokens(message.text);
        q->contextUpdated(estimated, it->contextLimit);
    }

    void renderChat()
    {
        clearChat();

        const NoteState *state = nullptr;
        auto it = m_notes.constFind(m_noteId);
        if (it != m_notes.constEnd())
            state = &*it;
        const Conversation *conv = state ? &conversation(*state, m_mode) : nullptr;

        if (!conv || conv->messages.isEmpty()) {
            Message hint;
            hint.hint = true;
            hint.harness = m_provider;
            hint.text = m_noteId.isEmpty()
                ? QStringLiteral("Open a note to start a conversation.")
                : (m_mode == Mode::Solo
                       ? QStringLiteral("What would you like to work on? I can give feedback or "
                                        "help brainstorm.")
                       : QStringLiteral("Give the four agents an idea or a question. They'll build "
                                        "on and challenge each other's suggestions."));
            addMessageRow(hint);
        } else {
            for (const Message &message : conv->messages)
                addMessageRow(message);
        }

        if (busy() && m_run.noteId == m_noteId && m_run.mode == m_mode) {
            Harness active = m_run.harness;
            if (m_run.phase == Phase::Council && m_run.position < m_run.order.size())
                active = m_run.order.at(m_run.position);
            const QString prefix =
                m_run.phase == Phase::Council ? QStringLiteral("Council · ") : QString();
            auto *status = new QWidget;
            auto *layout = new QHBoxLayout(status);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            layout->addWidget(new ReplyPulse, 0, Qt::AlignTop);
            auto *label = new QLabel(prefix + harnessName(active)
                                     + QStringLiteral(" is responding…"));
            label->setWordWrap(true);
            label->setStyleSheet(QStringLiteral(
                "color:#c0bbb3;background:transparent;font-size:11px;"));
            layout->addWidget(label, 1);
            addRendered(status);
        }

        relayoutRows();
        scrollToBottom();
    }

    void addMessageRow(const Message &message)
    {
        if (message.error) {
            auto *label = new QLabel(bounded(message.text, 4000));
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setContentsMargins(23, 0, 0, 0);
            label->setStyleSheet(
                QStringLiteral("color:%1;background:transparent;font-size:13px;")
                    .arg(message.error ? kPink.name() : kQuiet.name()));
            addRendered(label);
            return;
        }

        auto *row = new MessageRow(message.user);
        if (!message.user) {
            row->setAvatar(harnessIcon(harnessName(message.harness))
                               .pixmap(QSize(15, 15), q->devicePixelRatioF()));
            if (m_mode == Mode::Council && !message.hint) {
                QLabel *speaker = row->setSpeaker(harnessName(message.harness));
                QStringList details;
                if (!message.model.isEmpty() && message.model != QStringLiteral("Default"))
                    details << message.model;
                if (!message.effort.isEmpty() && message.effort != QStringLiteral("Default"))
                    details << message.effort;
                if (!details.isEmpty())
                    speaker->setToolTip(details.join(QStringLiteral(" · ")));
            }
        }

        if (message.quote.id) {
            row->addBody(QStringLiteral("↳ %1 · %2").arg(message.quote.speaker,
                         bounded(normalizedText(message.quote.text), 100)), kQuiet);
        }
        if (!message.text.isEmpty())
            row->addBody(message.text, message.user ? kPaper : kSoft);
        if (!message.images.isEmpty())
            row->addImages(message.images);

        if (!message.hint && !message.notice)
            row->addActions(buildActions(message));
        addRendered(row);
        m_rows.append(row);
    }

    QWidget *buildActions(const Message &message)
    {
        auto *actions = new QWidget;
        auto *layout = new QHBoxLayout(actions);
        layout->setContentsMargins(0, 1, 0, 0);
        layout->setSpacing(11);

        const QString noteId = m_noteId;
        const Mode mode = m_mode;
        const quint64 messageId = message.id;

        if (!message.jumpPassage.isEmpty() && q->jumpToPassage) {
            const bool available = normalizedText(m_noteText)
                    .contains(normalizedText(message.jumpPassage), Qt::CaseInsensitive);
            auto *button = makeActionButton(
                available ? QStringLiteral("Jump to passage")
                          : QStringLiteral("Passage unavailable"));
            button->setEnabled(available);
            connect(button, &QToolButton::clicked, this, [this, noteId, mode, messageId] {
                if (m_noteId != noteId || m_mode != mode)
                    return;
                Message *target = messageById(noteId, mode, messageId);
                if (!target || !q->jumpToPassage)
                    return;
                if (!normalizedText(m_noteText)
                         .contains(normalizedText(target->jumpPassage), Qt::CaseInsensitive)) {
                    return;
                }
                q->jumpToPassage(target->jumpPassage);
            });
            layout->addWidget(button);
        }

        if (!message.insertSuggestion.isEmpty() && q->insertText) {
            auto *button = makeActionButton(message.inserted
                                                ? QStringLiteral("Inserted")
                                                : (message.insertIsLine
                                                       ? QStringLiteral("Insert line")
                                                       : QStringLiteral("Insert in note")));
            button->setEnabled(!message.inserted);
            connect(button, &QToolButton::clicked, this, [this, noteId, mode, messageId] {
                if (m_noteId != noteId || m_mode != mode)
                    return;
                Message *target = messageById(noteId, mode, messageId);
                if (!target || target->inserted || !q->insertText)
                    return;
                target->inserted = true;
                q->insertText(target->insertSuggestion);
                renderChat();
            });
            layout->addWidget(button);
        }

        auto *reply = makeActionButton(QStringLiteral("Reply"));
        connect(reply, &QToolButton::clicked, this, [this, noteId, mode, messageId] {
            if (m_noteId != noteId || m_mode != mode)
                return;
            if (const auto *target = messageById(noteId, mode, messageId)) {
                m_reply = {messageId, speakerName(*target, mode), bounded(messageText(*target), 1600)};
                updateReplyBanner();
                m_input->setFocus();
            }
        });
        layout->addWidget(reply);
        layout->addStretch(1);
        return actions;
    }

    static QToolButton *makeActionButton(const QString &text)
    {
        auto *button = new QToolButton;
        button->setText(text);
        button->setCursor(Qt::PointingHandCursor);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setStyleSheet(QStringLiteral(
            "QToolButton{background:transparent;border:0;padding:0;color:#70cdef;"
            "font-size:12px;font-weight:700;}"
            "QToolButton:enabled:hover{color:#a5e7fc;background:#263740;border-radius:4px;}"
            "QToolButton:disabled{color:#a49f96;}"));
        return button;
    }

    void addRendered(QWidget *widget)
    {
        m_chatLayout->insertWidget(m_chatLayout->count() - 1, widget);
        m_rendered.append(widget);
    }

    void clearChat()
    {
        for (QWidget *widget : m_rendered) {
            m_chatLayout->removeWidget(widget);
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        m_rendered.clear();
        m_rows.clear();
    }

    void relayoutRows()
    {
        if (!m_scroll)
            return;
        const int width = m_scroll->viewport()->width();
        for (MessageRow *row : m_rows)
            row->setContentMaxWidth(width);
    }

    void scrollToBottom()
    {
        QTimer::singleShot(0, this, [this] {
            QScrollBar *bar = m_scroll->verticalScrollBar();
            bar->setValue(bar->maximum());
        });
    }
};

ChatPane::ChatPane(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("chatPane"));
    setMinimumWidth(210);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    QPalette colors = palette();
    colors.setColor(QPalette::Window, kRail);
    colors.setColor(QPalette::WindowText, kPaper);
    colors.setColor(QPalette::Base, kInk);
    colors.setColor(QPalette::Text, kPaper);
    colors.setColor(QPalette::Highlight, kBlue);
    colors.setColor(QPalette::HighlightedText, QColor("#14232a"));
    setPalette(colors);
    setAutoFillBackground(true);

    QFont baseFont = font();
    baseFont.setPixelSize(14);
    setFont(baseFont);

    m_impl = std::make_unique<Impl>(this);
}

ChatPane::~ChatPane() = default;

void ChatPane::setNote(const QString &id, const QString &title, const QString &text)
{
    if (m_impl)
        m_impl->setNote(id, title, text);
}

void ChatPane::renameNote(const QString &oldId, const QString &newId, const QString &title)
{
    m_impl->renameNote(oldId, newId, title);
}

void ChatPane::openSettings()
{
    m_impl->showSettings();
}

} // namespace sava
