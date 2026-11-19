#pragma once

#include "agents.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

#include <functional>

class QJsonObject;
class QProcess;

namespace sava {

struct ModelOption {
    QString id;
    QString label;
    QStringList efforts;
};

struct ModelList {
    QList<ModelOption> models;
    QStringList defaultEfforts;
    QString error;
};

struct QuotaWindow {
    double usedPercent = -1;
    int minutes = 0;
    QDateTime resetsAt;
};

struct ProviderQuota {
    QuotaWindow primary;
    QuotaWindow secondary;
};

// Queries installed harnesses for metadata only. The most recent successful
// inventory is kept in memory for five minutes; nothing runs at app startup.
class ModelCatalog final : public QObject
{
public:
    explicit ModelCatalog(QObject *parent = nullptr) : QObject(parent) {}
    void readQuota(Harness harness, std::function<void(const ProviderQuota &)> done);
    void refresh(Harness harness, std::function<void(const ModelList &)> done, bool force = false);

private:
    struct Cached {
        ModelList result;
        qint64 expires = 0;
    };

    void start();
    void readQuotaMessage(const QJsonObject &message);
    void finishQuota(const ProviderQuota &quota = {});
    void readOutput();
    void readMessage(const QJsonObject &message);
    void readListing();
    void send(const QJsonObject &message);
    void finish(QString error = {});
    void stop();

    QHash<int, Cached> cache;
    Harness harness = Harness::Codex;
    QProcess *process = nullptr;
    ProcessJob processJob;
    QByteArray output;
    qsizetype received = 0;
    ModelList pending;
    std::function<void(const ModelList &)> notify;
    std::function<void(const ProviderQuota &)> notifyQuota;
};

} // namespace sava
