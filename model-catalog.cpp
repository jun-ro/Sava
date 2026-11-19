#include "model-catalog.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

namespace sava {
namespace {

constexpr qsizetype MaxOutput = 2 * 1024 * 1024;
constexpr int TimeoutMs = 25000;

QStringList orderedEfforts(const QStringList &raw)
{
    static const QStringList kOrder = {
        QStringLiteral("off"), QStringLiteral("minimal"), QStringLiteral("low"),
        QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("xhigh"),
        QStringLiteral("max"), QStringLiteral("ultra")
    };
    QStringList out;
    for (const QString &level : kOrder)
        if (raw.contains(level)) out.append(level);
    for (const QString &level : raw)
        if (!level.isEmpty() && !out.contains(level)) out.append(level);
    return out;
}

QStringList reportedEfforts(const QJsonArray &entries)
{
    QStringList efforts;
    for (const auto &entry : entries) {
        const QString name = entry.isString() ? entry.toString()
            : entry.toObject().value("reasoningEffort").toString();
        if (!name.isEmpty()) efforts.append(name);
    }
    efforts.removeDuplicates();
    return efforts;
}

} // namespace

void ModelCatalog::refresh(Harness requested, std::function<void(const ModelList &)> done, bool force)
{
    if (process && !notifyQuota && harness == requested && !force) {
        notify = std::move(done);
        return;
    }
    stop();
    notifyQuota = {};
    harness = requested;
    const auto cached = cache.constFind(int(harness));
    if (!force && cached != cache.cend() && cached->expires > QDateTime::currentMSecsSinceEpoch()) {
        done(cached->result);
        return;
    }

    notify = std::move(done);
    start();
}

void ModelCatalog::readQuota(Harness requested, std::function<void(const ProviderQuota &)> done)
{
    stop();
    notify = {};
    notifyQuota = {};
    if (requested != Harness::Codex && requested != Harness::Claude) {
        done({});
        return;
    }
    harness = requested;
    notifyQuota = std::move(done);
    start();
}

void ModelCatalog::start()
{
    const QString program = Agents::findExecutable(harness);
    if (program.isEmpty()) {
        finish(Agents::displayName(harness) + " isn’t installed.");
        return;
    }
    pending = {};
    output.clear();
    received = 0;
    QStringList args;
    switch (harness) {
    case Harness::Codex:
        args = {"app-server", "--listen", "stdio://"};
        break;
    case Harness::Claude:
        args = {"-p", "--input-format", "stream-json", "--output-format", "stream-json",
                "--verbose", "--no-session-persistence", "--tools", "",
                "--strict-mcp-config", "--restricted"};
        break;
    case Harness::OpenCode:
        args = {"models", "--standalone"};
        break;
    case Harness::Pi:
        args = {"--list-models", "--no-extensions"};
        break;
    case Harness::Antigravity:
        break;
    }

    auto *job = new QProcess(this);
    process = job;
    Agents::configureCommand(job, program, args);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    Agents::applyHarnessEnvironment(harness, &environment);
    job->setProcessEnvironment(environment);
    job->setWorkingDirectory(QDir::tempPath());
    job->setStandardErrorFile(QProcess::nullDevice());
    connect(job, &QProcess::started, this, [this, job] {
        if (process == job) processJob.attach(job);
        switch (harness) {
        case Harness::Codex:
            send({{"id", 1}, {"method", "initialize"}, {"params", QJsonObject{
                {"clientInfo", QJsonObject{{"name", "sava"}, {"title", "Sava"}, {"version", "0.1"}}}
            }}});
            break;
        case Harness::Claude:
            send({{"type", "control_request"},
                  {"request_id", notifyQuota ? "sava-quota-init" : "sava-models"},
                  {"request", QJsonObject{{"subtype", "initialize"}}}});
            break;
        case Harness::Antigravity:
            send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{
                {"protocolVersion", 1},
                {"clientCapabilities", QJsonObject{}},
                {"clientInfo", QJsonObject{{"name", "sava"}, {"version", "0.1"}}}
            }}});
            break;
        default:
            process->closeWriteChannel();
        }
    });
    connect(job, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
    connect(job, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish("Couldn’t start the harness.");
    });
    connect(job, &QProcess::finished, this, [this, job](int code, QProcess::ExitStatus status) {
        readOutput();
        if (process != job) return;
        if (code == 0 && status == QProcess::NormalExit &&
            (harness == Harness::OpenCode || harness == Harness::Pi)) {
            readListing();
            finish();
        } else {
            finish("The harness didn’t return a model catalog.");
        }
    });
    QTimer::singleShot(TimeoutMs, job, [this, job] {
        if (process == job) finish("The model catalog timed out.");
    });
    job->start();
}

void ModelCatalog::send(const QJsonObject &message)
{
    process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void ModelCatalog::readOutput()
{
    if (!process) return;
    const QByteArray chunk = process->readAllStandardOutput();
    received += chunk.size();
    if (received > MaxOutput) {
        finish("The harness returned an oversized model catalog.");
        return;
    }
    output += chunk;
    if (harness == Harness::OpenCode || harness == Harness::Pi) return;
    while (process) {
        const auto newline = output.indexOf('\n');
        if (newline < 0) return;
        const auto message = QJsonDocument::fromJson(output.left(newline)).object();
        output.remove(0, newline + 1);
        readMessage(message);
    }
}

void ModelCatalog::readMessage(const QJsonObject &message)
{
    if (notifyQuota) {
        readQuotaMessage(message);
        return;
    }
    if (harness == Harness::Codex) {
        const int id = message.value("id").toInt();
        if (id != 1 && id != 2) return;
        const auto result = message.value("result").toObject();
        if (result.isEmpty()) { finish("Codex couldn’t list its models."); return; }
        QJsonObject params{{"includeHidden", false}, {"limit", 50}};
        if (id == 1) {
            send({{"method", "initialized"}});
        } else {
            for (const auto &entry : result.value("data").toArray()) {
                const auto row = entry.toObject();
                const auto id = row.value("model").toString(row.value("id").toString());
                const auto levels = reportedEfforts(row.value("supportedReasoningEfforts").toArray());
                pending.models.append({id, row.value("displayName").toString(id), levels});
                if (row.value("isDefault").toBool()) pending.defaultEfforts = levels;
            }
            const QString cursor = result.value("nextCursor").toString();
            if (cursor.isEmpty()) { finish(); return; }
            params.insert("cursor", cursor);
        }
        send({{"id", 2}, {"method", "model/list"}, {"params", params}});
        return;
    }

    if (harness == Harness::Antigravity) {
        const int id = message.value("id").toInt();
        if (id != 1 && id != 2) return;
        if (message.contains("error") && !message.value("error").isNull()) {
            const QString err = message.value("error").toObject().value("message").toString().trimmed();
            finish(err.isEmpty() ? QStringLiteral("Antigravity couldn’t list its models.")
                                 : QStringLiteral("Antigravity: ") + err);
            return;
        }
        const auto result = message.value("result").toObject();
        if (result.isEmpty()) { finish("Antigravity couldn’t list its models."); return; }
        if (id == 1) {
            send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "session/new"}, {"params", QJsonObject{
                {"cwd", QDir::toNativeSeparators(QDir::tempPath())},
                {"mcpServers", QJsonArray{}}
            }}});
            return;
        }

        QJsonArray modelConfigOptions;
        QString currentModel;
        QStringList fallbackEfforts;
        for (const auto &entry : result.value("configOptions").toArray()) {
            const auto opt = entry.toObject();
            const QString optId = opt.value("id").toString();
            if (optId == QLatin1String("model")) {
                modelConfigOptions = opt.value("options").toArray();
                currentModel = opt.value("currentValue").toString();
            } else if (optId == QLatin1String("thinking_level")) {
                for (const auto &level : opt.value("options").toArray()) {
                    const QString val = level.toObject().value("value").toString().trimmed().toLower();
                    if (!val.isEmpty()) fallbackEfforts.append(val);
                }
            }
        }
        fallbackEfforts = orderedEfforts(fallbackEfforts);

        static const QStringList kKnownSuffixes = {
            QStringLiteral("low"), QStringLiteral("medium"),
            QStringLiteral("high"), QStringLiteral("xhigh")
        };
        QHash<QString, QStringList> effortsByModel;
        QList<QPair<QString, QString>> discoveredModels;
        for (const auto &entry : result.value("models").toObject().value("availableModels").toArray()) {
            const auto row = entry.toObject();
            const QString rawId = row.value("modelId").toString(row.value("id").toString()).trimmed();
            if (rawId.isEmpty()) continue;
            QString base = rawId;
            QString suffix;
            for (const QString &candidate : kKnownSuffixes) {
                const QString tag = QLatin1Char('-') + candidate;
                if (rawId.endsWith(tag, Qt::CaseInsensitive)) {
                    base = rawId.left(rawId.size() - tag.size());
                    suffix = candidate;
                    break;
                }
            }
            if (!suffix.isEmpty())
                effortsByModel[base].append(suffix);
            else if (!effortsByModel.contains(base))
                effortsByModel.insert(base, {});
            discoveredModels.append({base, row.value("name").toString(base)});
        }

        if (!modelConfigOptions.isEmpty()) {
            for (const auto &entry : modelConfigOptions) {
                const auto row = entry.toObject();
                const QString value = row.value("value").toString().trimmed();
                if (value.isEmpty()) continue;
                const QString label = row.value("name").toString(value).trimmed();
                QStringList levels = orderedEfforts(effortsByModel.value(value));
                if (levels.isEmpty()) levels = fallbackEfforts;
                pending.models.append({value, label.isEmpty() ? value : label, levels});
                if (value == currentModel || pending.defaultEfforts.isEmpty())
                    pending.defaultEfforts = levels;
            }
        } else {
            for (const auto &pair : discoveredModels) {
                QStringList levels = orderedEfforts(effortsByModel.value(pair.first));
                if (levels.isEmpty()) levels = fallbackEfforts;
                pending.models.append({pair.first, pair.second, levels});
                if (pending.defaultEfforts.isEmpty())
                    pending.defaultEfforts = levels;
            }
        }
        finish();
        return;
    }

    const auto response = message.value("response").toObject();
    if (response.value("request_id").toString() != "sava-models") return;
    for (const auto &entry : response.value("response").toObject().value("models").toArray()) {
        const auto row = entry.toObject();
        const auto id = row.value("value").toString();
        const auto levels = reportedEfforts(row.value("supportedEffortLevels").toArray());
        if (id == "default") pending.defaultEfforts = levels;
        else pending.models.append({id, row.value("displayName").toString(id), levels});
    }
    finish();
}

void ModelCatalog::readQuotaMessage(const QJsonObject &message)
{
    if (harness == Harness::Codex) {
        const int id = message.value("id").toInt();
        const auto result = message.value("result").toObject();
        if (id == 1) {
            if (result.isEmpty()) {
                finishQuota();
                return;
            }
            send({{"method", "initialized"}});
            send({{"method", "account/rateLimits/read"}, {"id", 2}});
        } else if (id == 2) {
            auto limits = result.value("rateLimitsByLimitId").toObject().value("codex").toObject();
            if (limits.isEmpty()) limits = result.value("rateLimits").toObject();
            const auto window = [&limits](const char *key) {
                const auto data = limits.value(QLatin1String(key)).toObject();
                const auto reset = data.value("resetsAt").toInteger();
                return QuotaWindow{data.value("usedPercent").toDouble(-1),
                                   data.value("windowDurationMins").toInt(),
                                   reset > 0 ? QDateTime::fromSecsSinceEpoch(reset) : QDateTime()};
            };
            finishQuota({window("primary"), window("secondary")});
        }
        return;
    }

    const auto response = message.value("response").toObject();
    const auto id = response.value("request_id").toString();
    if (id == "sava-quota-init") {
        if (response.value("subtype").toString() != "success") {
            finishQuota();
            return;
        }
        send({{"type", "control_request"}, {"request_id", "sava-quota"},
              {"request", QJsonObject{{"subtype", "get_usage"}}}});
    } else if (id == "sava-quota") {
        const auto limits = response.value("response").toObject().value("rate_limits").toObject();
        const auto window = [&limits](const char *key, int minutes) {
            const auto data = limits.value(QLatin1String(key)).toObject();
            return QuotaWindow{data.value("utilization").toDouble(-1), minutes,
                               QDateTime::fromString(data.value("resets_at").toString(), Qt::ISODate)};
        };
        finishQuota({window("five_hour", 5 * 60), window("seven_day", 7 * 24 * 60)});
    }
}

void ModelCatalog::finishQuota(const ProviderQuota &quota)
{
    stop();
    auto done = std::move(notifyQuota);
    notifyQuota = {};
    done(quota);
}

void ModelCatalog::readListing()
{
    static const QRegularExpression whiteSpace("\\s+");
    static const QRegularExpression modelId("^[A-Za-z0-9][A-Za-z0-9._-]*/[^\\s/][^\\s]*$");
    for (const auto &line : QString::fromUtf8(output).split('\n')) {
        QString id = line.trimmed();
        QStringList efforts;
        if (harness == Harness::Pi) {
            const auto cols = id.split(whiteSpace, Qt::SkipEmptyParts);
            if (cols.size() < 5 || (cols[4] != "yes" && cols[4] != "no")) continue;
            id = cols[0] + '/' + cols[1];
            if (cols[4] == "yes") efforts = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
        } else {
            efforts = {"low", "medium", "high", "max"};
        }
        if (modelId.match(id).hasMatch()) pending.models.append({id, id, efforts});
    }
}

void ModelCatalog::finish(QString error)
{
    if (notifyQuota) {
        finishQuota();
        return;
    }
    stop();
    QSet<QString> seen;
    pending.models.removeIf([&seen](const ModelOption &model) {
        if (model.id.isEmpty() || seen.contains(model.id)) return true;
        seen.insert(model.id);
        return false;
    });
    const auto key = int(harness);
    if (error.isEmpty() && pending.models.isEmpty()) error = "No models were returned. Check the harness connection.";
    if (error.isEmpty()) {
        cache.insert(key, Cached{pending, QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000});
    } else {
        const auto previous = cache.constFind(key);
        if (previous != cache.cend()) pending = previous->result;
        pending.error = error;
    }
    const auto done = std::move(notify);
    if (done) done(pending);
}

void ModelCatalog::stop()
{
    auto *job = process;
    process = nullptr;
    if (!job) {
        processJob.release();
        return;
    }
    job->disconnect(this);
    if (job->state() == QProcess::NotRunning) {
        processJob.release();
        job->deleteLater();
    } else {
        processJob.terminateAndClose();
        connect(job, &QProcess::finished, job, &QObject::deleteLater);
        job->kill();
    }
}

} // namespace sava
