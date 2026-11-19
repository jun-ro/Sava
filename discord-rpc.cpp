#include "discord-rpc.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstring>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace sava {
namespace {

constexpr quint32 kOpHandshake = 0;
constexpr quint32 kOpFrame = 1;
constexpr quint32 kOpClose = 2;
constexpr quint32 kOpPing = 3;
constexpr quint32 kOpPong = 4;

constexpr int kPollIntervalMs = 500;
constexpr int kReconnectIntervalMs = 8000;
constexpr int kDebounceMs = 3000;
constexpr int kMinPushGapMs = 1500;
constexpr quint32 kMaxFrameBytes = 256 * 1024;
constexpr int kStatusDisplayDetails = 2;

bool isValidClientId(const QString &id)
{
    if (id.size() < 15 || id.size() > 25)
        return false;
    for (const QChar ch : id) {
        if (!ch.isDigit())
            return false;
    }
    return true;
}

QString clampField(QString text)
{
    text = text.trimmed();
    if (text.size() > 128)
        text = text.left(127) + QChar(0x2026);
    if (text.size() == 1)
        text += QLatin1Char(' ');
    return text;
}

void writeLe32(char *dst, quint32 value)
{
    dst[0] = static_cast<char>(value & 0xffu);
    dst[1] = static_cast<char>((value >> 8) & 0xffu);
    dst[2] = static_cast<char>((value >> 16) & 0xffu);
    dst[3] = static_cast<char>((value >> 24) & 0xffu);
}

quint32 readLe32(const char *src)
{
    return static_cast<quint32>(static_cast<unsigned char>(src[0]))
        | (static_cast<quint32>(static_cast<unsigned char>(src[1])) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(src[2])) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(src[3])) << 24);
}

#ifdef Q_OS_WIN
bool readExactWin(HANDLE pipe, char *buffer, DWORD size)
{
    DWORD total = 0;
    while (total < size) {
        DWORD got = 0;
        if (!::ReadFile(pipe, buffer + total, size - total, &got, nullptr) || got == 0)
            return false;
        total += got;
    }
    return true;
}

bool writeExactWin(HANDLE pipe, const char *buffer, DWORD size)
{
    DWORD total = 0;
    while (total < size) {
        DWORD wrote = 0;
        if (!::WriteFile(pipe, buffer + total, size - total, &wrote, nullptr) || wrote == 0)
            return false;
        total += wrote;
    }
    return true;
}
#else
bool readExactPosix(int fd, char *buffer, size_t size)
{
    size_t total = 0;
    while (total < size) {
        const ssize_t got = ::read(fd, buffer + total, size - total);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        total += static_cast<size_t>(got);
    }
    return true;
}

bool writeExactPosix(int fd, const char *buffer, size_t size)
{
    size_t total = 0;
    while (total < size) {
        const ssize_t wrote = ::write(fd, buffer + total, size - total);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (wrote == 0)
            return false;
        total += static_cast<size_t>(wrote);
    }
    return true;
}
#endif

} // namespace

DiscordPresence::DiscordPresence(QObject *parent)
    : QObject(parent)
    , m_startedAt(QDateTime::currentSecsSinceEpoch())
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, [this] {
        if (!isConnected()) {
            m_pollTimer->stop();
            return;
        }
        if (!readAvailableFrames()) {
            closeTransport(false);
            if (m_enabled && isValidClientId(effectiveClientId())) {
                setStatus(QStringLiteral("Waiting for Discord…"));
                m_reconnectTimer->start();
            }
        }
    });

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(kReconnectIntervalMs);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this] { tryConnect(); });

    m_pushTimer = new QTimer(this);
    m_pushTimer->setSingleShot(true);
    connect(m_pushTimer, &QTimer::timeout, this, [this] { pushActivity(); });
}

DiscordPresence::~DiscordPresence()
{
    shutdown();
}

void DiscordPresence::setStatus(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    if (statusChanged)
        statusChanged(m_status);
}

QString DiscordPresence::effectiveClientId() const
{
    const QString trimmed = m_clientId.trimmed();
    if (!trimmed.isEmpty())
        return trimmed;
    return qEnvironmentVariable("SAVA_DISCORD_CLIENT_ID").trimmed();
}

bool DiscordPresence::isConnected() const
{
#ifdef Q_OS_WIN
    return m_pipe != nullptr && m_pipe != INVALID_HANDLE_VALUE;
#else
    return m_socket >= 0;
#endif
}

void DiscordPresence::configure(bool enabled, const QString &clientId, bool showNoteTitle)
{
    const QString previousEffective = effectiveClientId();
    const bool privacyChanged = (m_showNoteTitle != showNoteTitle);
    m_enabled = enabled;
    m_clientId = clientId.trimmed();
    m_showNoteTitle = showNoteTitle;

    const QString activeId = effectiveClientId();
    if (!m_enabled) {
        m_reconnectTimer->stop();
        m_pushTimer->stop();
        clearActivity();
        closeTransport(true);
        setStatus(QStringLiteral("Disabled"));
        return;
    }

    if (!isValidClientId(activeId)) {
        m_reconnectTimer->stop();
        m_pushTimer->stop();
        clearActivity();
        closeTransport(true);
        setStatus(activeId.isEmpty()
                      ? QStringLiteral("Enter a Discord Application ID to connect")
                      : QStringLiteral("Invalid Application ID (numeric ID from Discord Developer Portal)"));
        return;
    }

    if (isConnected() && activeId != m_connectedClientId) {
        clearActivity();
        closeTransport(true);
    }

    if (!isConnected()) {
        if (m_startedAt == 0)
            m_startedAt = QDateTime::currentSecsSinceEpoch();
        tryConnect();
        if (!isConnected())
            m_reconnectTimer->start();
    } else if (privacyChanged || activeId != previousEffective) {
        m_lastDetails.clear();
        m_lastState.clear();
        m_dirty = true;
        schedulePush(true);
    }
}

void DiscordPresence::setActivity(const QString &noteTitle, const QString &folderName,
                                  qsizetype wordCount, bool previewing)
{
    const bool majorChange = (noteTitle != m_noteTitle)
        || (folderName != m_folderName)
        || (previewing != m_previewing);
    const bool wordChange = (wordCount != m_wordCount);
    m_noteTitle = noteTitle;
    m_folderName = folderName;
    m_wordCount = wordCount;
    m_previewing = previewing;

    if (!majorChange && !wordChange)
        return;

    m_dirty = true;
    if (m_enabled && m_ready)
        schedulePush(majorChange);
}

void DiscordPresence::shutdown()
{
    if (m_pollTimer)
        m_pollTimer->stop();
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (m_pushTimer)
        m_pushTimer->stop();
    clearActivity();
    closeTransport(true);
}

bool DiscordPresence::openTransport()
{
#ifdef Q_OS_WIN
    for (int index = 0; index < 10; ++index) {
        const QString pipeName = QStringLiteral(R"(\\.\pipe\discord-ipc-%1)").arg(index);
        const std::wstring wide = pipeName.toStdWString();
        HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            m_pipe = handle;
            return true;
        }
    }
    return false;
#else
    QStringList dirs;
    for (const char *var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        const QString value = qEnvironmentVariable(var).trimmed();
        if (!value.isEmpty() && !dirs.contains(value))
            dirs.append(value);
    }
    if (!dirs.contains(QStringLiteral("/tmp")))
        dirs.append(QStringLiteral("/tmp"));

    for (const QString &base : dirs) {
        for (const QString &sub : {QString(), QStringLiteral("/app/com.discordapp.Discord"),
                                   QStringLiteral("/snap.discord")}) {
            for (int index = 0; index < 10; ++index) {
                const QString path = base + sub + QStringLiteral("/discord-ipc-%1").arg(index);
                const QByteArray encoded = path.toUtf8();
                sockaddr_un addr{};
                if (encoded.size() >= static_cast<int>(sizeof(addr.sun_path)))
                    continue;
                const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0)
                    continue;
                addr.sun_family = AF_UNIX;
                std::memcpy(addr.sun_path, encoded.constData(), static_cast<size_t>(encoded.size()));
                if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
                    m_socket = fd;
                    return true;
                }
                ::close(fd);
            }
        }
    }
    return false;
#endif
}

void DiscordPresence::closeTransport(bool sendCloseFrame)
{
    if (sendCloseFrame && isConnected())
        writeFrame(kOpClose, QByteArrayLiteral("{}"));

    m_ready = false;
    m_connectedClientId.clear();
    m_lastDetails.clear();
    m_lastState.clear();
    if (m_pollTimer)
        m_pollTimer->stop();

#ifdef Q_OS_WIN
    if (m_pipe && m_pipe != INVALID_HANDLE_VALUE)
        ::CloseHandle(static_cast<HANDLE>(m_pipe));
    m_pipe = nullptr;
#else
    if (m_socket >= 0)
        ::close(m_socket);
    m_socket = -1;
#endif
}

bool DiscordPresence::writeFrame(quint32 opcode, const QByteArray &payload)
{
    if (!isConnected())
        return false;
    QByteArray packet(8 + payload.size(), '\0');
    writeLe32(packet.data(), opcode);
    writeLe32(packet.data() + 4, static_cast<quint32>(payload.size()));
    if (!payload.isEmpty())
        std::memcpy(packet.data() + 8, payload.constData(), static_cast<size_t>(payload.size()));

#ifdef Q_OS_WIN
    return writeExactWin(static_cast<HANDLE>(m_pipe), packet.constData(),
                         static_cast<DWORD>(packet.size()));
#else
    return writeExactPosix(m_socket, packet.constData(), static_cast<size_t>(packet.size()));
#endif
}

bool DiscordPresence::readAvailableFrames()
{
    if (!isConnected())
        return false;

    for (int guard = 0; guard < 16; ++guard) {
#ifdef Q_OS_WIN
        DWORD available = 0;
        if (!::PeekNamedPipe(static_cast<HANDLE>(m_pipe), nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (available < 8)
            return true;
        char header[8];
        if (!readExactWin(static_cast<HANDLE>(m_pipe), header, 8))
            return false;
        const quint32 opcode = readLe32(header);
        const quint32 length = readLe32(header + 4);
        if (length > kMaxFrameBytes)
            return false;
        QByteArray payload(static_cast<int>(length), '\0');
        if (length > 0 && !readExactWin(static_cast<HANDLE>(m_pipe), payload.data(), length))
            return false;
        handleFrame(opcode, payload);
        if (!isConnected())
            return true;
#else
        pollfd pfd{};
        pfd.fd = m_socket;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 0);
        if (ready < 0)
            return errno == EINTR;
        if (ready == 0)
            return true;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return false;
        char header[8];
        if (!readExactPosix(m_socket, header, 8))
            return false;
        const quint32 opcode = readLe32(header);
        const quint32 length = readLe32(header + 4);
        if (length > kMaxFrameBytes)
            return false;
        QByteArray payload(static_cast<int>(length), '\0');
        if (length > 0 && !readExactPosix(m_socket, payload.data(), length))
            return false;
        handleFrame(opcode, payload);
        if (!isConnected())
            return true;
#endif
    }
    return true;
}

void DiscordPresence::handleFrame(quint32 opcode, const QByteArray &payload)
{
    if (opcode == kOpPing) {
        writeFrame(kOpPong, payload);
        return;
    }
    if (opcode == kOpClose) {
        const QJsonObject obj = QJsonDocument::fromJson(payload).object();
        const QString msg = obj.value(QStringLiteral("message")).toString();
        closeTransport(false);
        if (m_enabled) {
            setStatus(msg.isEmpty()
                          ? QStringLiteral("Waiting for Discord…")
                          : QStringLiteral("Discord closed: %1").arg(msg));
            m_reconnectTimer->start();
        }
        return;
    }
    if (opcode != kOpFrame)
        return;

    const QJsonObject obj = QJsonDocument::fromJson(payload).object();
    const QString evt = obj.value(QStringLiteral("evt")).toString();
    if (evt == QStringLiteral("READY")) {
        m_ready = true;
        const QString user = obj.value(QStringLiteral("data")).toObject()
                                 .value(QStringLiteral("user")).toObject()
                                 .value(QStringLiteral("username")).toString();
        setStatus(user.isEmpty()
                      ? QStringLiteral("Connected to Discord")
                      : QStringLiteral("Connected to Discord (@%1)").arg(user));
        m_dirty = true;
        pushActivity();
    } else if (evt == QStringLiteral("ERROR")) {
        const QString msg = obj.value(QStringLiteral("data")).toObject()
                                .value(QStringLiteral("message")).toString();
        if (!msg.isEmpty())
            setStatus(QStringLiteral("Discord error: %1").arg(msg));
    }
}

void DiscordPresence::tryConnect()
{
    if (!m_enabled || isConnected())
        return;
    const QString clientId = effectiveClientId();
    if (!isValidClientId(clientId))
        return;

    if (!openTransport()) {
        setStatus(QStringLiteral("Waiting for Discord…"));
        return;
    }

    const QJsonObject handshake{
        {QStringLiteral("v"), 1},
        {QStringLiteral("client_id"), clientId}
    };
    if (!writeFrame(kOpHandshake, QJsonDocument(handshake).toJson(QJsonDocument::Compact))) {
        closeTransport(false);
        setStatus(QStringLiteral("Waiting for Discord…"));
        return;
    }

    m_connectedClientId = clientId;
    m_reconnectTimer->stop();
    setStatus(QStringLiteral("Connecting to Discord…"));
    m_pollTimer->start();
    readAvailableFrames();
}

void DiscordPresence::schedulePush(bool immediate)
{
    if (!m_enabled || !m_ready)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = now - m_lastPushMs;
    if (immediate && elapsed >= kMinPushGapMs) {
        m_pushTimer->stop();
        pushActivity();
        return;
    }
    const int waitMs = immediate ? static_cast<int>(kMinPushGapMs - elapsed) : kDebounceMs;
    if (!m_pushTimer->isActive() || waitMs < m_pushTimer->remainingTime())
        m_pushTimer->start(qMax(100, waitMs));
}

void DiscordPresence::pushActivity()
{
    if (!m_enabled || !m_ready || !isConnected())
        return;

    QString details;
    QString state;
    if (m_noteTitle.isEmpty()) {
        details = QStringLiteral("Browsing notes");
        state = QStringLiteral("In Sava");
    } else {
        const QString verb = m_previewing ? QStringLiteral("Previewing") : QStringLiteral("Writing");
        details = m_showNoteTitle
            ? QStringLiteral("%1 %2").arg(verb, m_noteTitle)
            : QStringLiteral("%1 a note").arg(verb);

        const QString wordText = QString::number(m_wordCount)
            + (m_wordCount == 1 ? QStringLiteral(" word") : QStringLiteral(" words"));
        state = (m_showNoteTitle && !m_folderName.isEmpty())
            ? QStringLiteral("%1 · %2").arg(wordText, m_folderName)
            : wordText;
    }

    details = clampField(details);
    state = clampField(state);
    if (!m_dirty && details == m_lastDetails && state == m_lastState)
        return;

    const QJsonObject activity{
        {QStringLiteral("type"), 0},
        {QStringLiteral("status_display_type"), kStatusDisplayDetails},
        {QStringLiteral("details"), details},
        {QStringLiteral("state"), state},
        {QStringLiteral("timestamps"), QJsonObject{{QStringLiteral("start"), m_startedAt}}},
        {QStringLiteral("assets"), QJsonObject{
            {QStringLiteral("large_image"), QLatin1String(kDiscordPresenceImageAsset)},
            {QStringLiteral("large_text"), QStringLiteral("Sava")}
        }}
    };
    const QJsonObject command{
        {QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
        {QStringLiteral("args"), QJsonObject{
            {QStringLiteral("pid"), QCoreApplication::applicationPid()},
            {QStringLiteral("activity"), activity}
        }},
        {QStringLiteral("nonce"), QString::number(m_nonce++)}
    };

    if (!writeFrame(kOpFrame, QJsonDocument(command).toJson(QJsonDocument::Compact))) {
        closeTransport(false);
        setStatus(QStringLiteral("Waiting for Discord…"));
        m_reconnectTimer->start();
        return;
    }

    m_lastDetails = details;
    m_lastState = state;
    m_dirty = false;
    m_lastPushMs = QDateTime::currentMSecsSinceEpoch();
}

void DiscordPresence::clearActivity()
{
    if (!m_ready || !isConnected())
        return;
    const QJsonObject command{
        {QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
        {QStringLiteral("args"), QJsonObject{
            {QStringLiteral("pid"), QCoreApplication::applicationPid()}
        }},
        {QStringLiteral("nonce"), QString::number(m_nonce++)}
    };
    writeFrame(kOpFrame, QJsonDocument(command).toJson(QJsonDocument::Compact));
}

} // namespace sava
