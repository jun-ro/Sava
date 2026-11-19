#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>

#include <functional>

class QJsonObject;
class QProcess;
class QProcessEnvironment;
class QTemporaryDir;
class QTimer;

namespace sava {

enum class Harness { Codex, Claude, OpenCode, Pi, Antigravity };

struct AgentRequest {
    Harness harness;
    QString model;
    QString effort;
    QString prompt;
    QStringList images;
    bool webSearch = false;
};

struct AgentReply {
    bool ok = false;
    QString text;
    QString error;
    qint64 inputTokens = -1;
    qint64 outputTokens = -1;
    qint64 contextLimit = -1;
};

struct ProcessJob {
    void *handle = nullptr;

    ProcessJob() = default;
    ~ProcessJob();
    ProcessJob(const ProcessJob &) = delete;
    ProcessJob &operator=(const ProcessJob &) = delete;

    void attach(QProcess *process);
    void terminateAndClose();
    void release();
};

class Agents : public QObject {
public:
    explicit Agents(QObject *parent = nullptr);
    ~Agents() override;

    static QString executable(Harness harness);
    static QString findExecutable(Harness harness);
    static void applyHarnessEnvironment(Harness harness, QProcessEnvironment *environment);
    static void configureCommand(QProcess *process, const QString &program,
                                 const QStringList &arguments);
    static QString displayName(Harness harness);
    static bool supportsWebSearch(Harness harness);

    // Returns false only when a request is already running or no callback was
    // supplied. Every accepted request gets exactly one asynchronous callback,
    // including when it is cancelled.
    bool start(const AgentRequest &request, std::function<void(AgentReply)> callback);
    void cancel();

private:
    void complete(AgentReply reply);
    void killProcess();
    void readOutput();
    void finishProcess();
    void failLater(const QString &error);
    void sendAcpMessage(const QJsonObject &message);
    void handleAcpLine(const QByteArray &line);
    AgentReply parseReply() const;

    bool m_busy = false;
    quint64 m_generation = 0;
    QProcess *m_process = nullptr;
    ProcessJob m_job;
    QTimer *m_timeout = nullptr;
    QTemporaryDir *m_workdir = nullptr;
    std::function<void(AgentReply)> m_callback;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QString m_stopReason;
    QString m_lastMessagePath;
    Harness m_harness = Harness::Codex;

    QByteArray m_acpBuffer;
    QString m_acpSessionId;
    QString m_acpText;
    QString m_acpInstructions;
    QString m_acpModel;
    QString m_acpEffort;
    QString m_acpPrompt;
    QStringList m_acpImages;
    bool m_acpWebSearch = false;
    int m_acpPromptId = 0;
    qint64 m_acpContextLimit = -1;
};

} // namespace sava
