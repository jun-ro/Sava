#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <functional>

class QTimer;

namespace sava {

inline constexpr char kDiscordPresenceImageAsset[] = "sava-chalk-mockup";

class DiscordPresence final : public QObject
{
public:
    explicit DiscordPresence(QObject *parent = nullptr);
    ~DiscordPresence() override;

    void configure(bool enabled, const QString &clientId, bool showNoteTitle);
    void setActivity(const QString &noteTitle, const QString &folderName,
                     qsizetype wordCount, bool previewing);
    void shutdown();

    QString statusText() const { return m_status; }
    std::function<void(const QString &)> statusChanged;

private:
    void setStatus(const QString &text);
    QString effectiveClientId() const;
    bool isConnected() const;
    bool openTransport();
    void closeTransport(bool sendCloseFrame);
    bool writeFrame(quint32 opcode, const QByteArray &payload);
    bool readAvailableFrames();
    void handleFrame(quint32 opcode, const QByteArray &payload);
    void tryConnect();
    void schedulePush(bool immediate);
    void pushActivity();
    void clearActivity();

    QTimer *m_pollTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    QTimer *m_pushTimer = nullptr;

    bool m_enabled = false;
    bool m_showNoteTitle = true;
    QString m_clientId;
    QString m_connectedClientId;
    bool m_ready = false;
    QString m_status = QStringLiteral("Disabled");

    QString m_noteTitle;
    QString m_folderName;
    qsizetype m_wordCount = 0;
    bool m_previewing = false;
    bool m_dirty = false;
    QString m_lastDetails;
    QString m_lastState;
    qint64 m_startedAt = 0;
    qint64 m_lastPushMs = 0;
    quint64 m_nonce = 1;

#ifdef Q_OS_WIN
    void *m_pipe = nullptr;
#else
    int m_socket = -1;
#endif
};

} // namespace sava
