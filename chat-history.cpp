#include "chat-history.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

namespace sava {
namespace {

constexpr auto kHistoryFile = "chat.json";
constexpr auto kPrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

QByteArray revision(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool writePrivateFile(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(kPrivateFile)
        && file.write(data) == data.size() && file.commit();
}

bool isImageName(const QString &name)
{
    const auto uuid = QUuid::fromString(name.chopped(4));
    return !uuid.isNull() && name == uuid.toString(QUuid::WithoutBraces) + QStringLiteral(".jpg");
}

Harness storedHarness(const QJsonValue &value)
{
    const auto name = value.toString();
    for (const auto harness : {Harness::Codex, Harness::Claude, Harness::OpenCode,
                               Harness::Pi, Harness::Antigravity}) {
        if (Agents::displayName(harness) == name) return harness;
    }
    return Harness::Codex;
}

Conversation readConversation(const QJsonObject &object, const QDir &directory)
{
    Conversation conversation;
    conversation.lastPromptTokens = object.value("lastPromptTokens").toInteger(-1);
    const auto messages = object.value("messages").toArray();
    for (qsizetype i = qMax<qsizetype>(0, messages.size() - kMaxChatMessages); i < messages.size(); ++i) {
        const auto entry = messages.at(i).toObject();
        const auto id = entry.value("id").toInteger();
        if (id <= 0) continue;

        Message message;
        message.id = quint64(id);
        message.user = entry.value("user").toBool();
        message.notice = entry.value("notice").toBool();
        message.error = entry.value("error").toBool();
        message.text = entry.value("text").toString();
        message.harness = storedHarness(entry.value("harness"));
        message.model = entry.value("model").toString();
        message.effort = entry.value("effort").toString();
        message.jumpPassage = entry.value("jumpPassage").toString();
        message.insertSuggestion = entry.value("insertSuggestion").toString();
        message.insertIsLine = entry.value("insertIsLine").toBool();
        message.inserted = entry.value("inserted").toBool();
        const auto quote = entry.value("quote").toObject();
        const auto quoteId = quote.value("id").toInteger();
        if (quoteId > 0)
            message.quote = {quint64(quoteId), quote.value("speaker").toString(), quote.value("text").toString()};

        const auto images = entry.value("images").toArray();
        for (qsizetype index = 0; index < qMin<qsizetype>(images.size(), kMaxChatImages); ++index) {
            const auto name = images.at(index).toString();
            if (!isImageName(name)) continue;
            QImageReader reader(directory.filePath(name), "JPEG");
            const QSize size = reader.size();
            if (size.isEmpty() || size.width() > kChatImageSide || size.height() > kChatImageSide)
                continue;
            reader.setScaledSize(size.scaled(kChatThumbnailSide, kChatThumbnailSide, Qt::KeepAspectRatio));
            const auto thumbnail = reader.read();
            if (!thumbnail.isNull())
                message.images.append({{}, QPixmap::fromImage(thumbnail), name});
        }
        conversation.messages.append(std::move(message));
    }
    return conversation;
}

QJsonObject writeConversation(Conversation &conversation, const QDir &directory,
                              QSet<QString> &referencedImages, bool &imagesSaved)
{
    QJsonArray messages;
    for (auto &message : conversation.messages) {
        QJsonObject entry{{"id", qint64(message.id)}, {"text", message.text}};
        if (message.user) entry.insert("user", true);
        if (message.notice) entry.insert("notice", true);
        if (message.error) entry.insert("error", true);
        if (!message.user && !message.notice) entry.insert("harness", Agents::displayName(message.harness));
        if (!message.model.isEmpty()) entry.insert("model", message.model);
        if (!message.effort.isEmpty()) entry.insert("effort", message.effort);
        if (!message.jumpPassage.isEmpty()) entry.insert("jumpPassage", message.jumpPassage);
        if (!message.insertSuggestion.isEmpty()) entry.insert("insertSuggestion", message.insertSuggestion);
        if (message.insertIsLine) entry.insert("insertIsLine", true);
        if (message.inserted) entry.insert("inserted", true);
        if (message.quote.id) {
            entry.insert("quote", QJsonObject{{"id", qint64(message.quote.id)},
                                             {"speaker", message.quote.speaker},
                                             {"text", message.quote.text}});
        }

        QJsonArray images;
        for (auto &image : message.images) {
            if (image.storedName.isEmpty() && image.file) {
                const auto name = QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".jpg");
                QFile source(image.file->fileName());
                const auto imageData = source.open(QIODevice::ReadOnly) ? source.readAll() : QByteArray{};
                if (!imageData.isEmpty() && source.error() == QFileDevice::NoError
                    && writePrivateFile(directory.filePath(name), imageData)) {
                    image.storedName = name;
                    image.file.reset();
                } else {
                    imagesSaved = false;
                }
            }
            if (!image.storedName.isEmpty()) {
                images.append(image.storedName);
                referencedImages.insert(image.storedName);
            }
        }
        if (!images.isEmpty()) entry.insert("images", images);
        messages.append(entry);
    }
    return {{"lastPromptTokens", conversation.lastPromptTokens}, {"messages", messages}};
}

// Version 1 stored one conversation per mode; version 2 stores a list of sessions.
SessionSet readSessions(const QJsonObject &object, const QDir &directory, int version)
{
    SessionSet set;
    if (version < 2) {
        set.list = {readConversation(object, directory)};
        return set;
    }
    set.list.clear();
    const auto sessions = object.value("sessions").toArray();
    for (qsizetype i = qMax<qsizetype>(0, sessions.size() - kMaxChatSessions); i < sessions.size(); ++i)
        set.list.append(readConversation(sessions.at(i).toObject(), directory));
    if (set.list.isEmpty()) set.list.append(Conversation{});
    // `current` counts from the start of the stored list, which may have been cut above.
    const int dropped = int(qMax<qsizetype>(0, sessions.size() - kMaxChatSessions));
    set.current = qBound(0, int(object.value("current").toInteger(0)) - dropped, int(set.list.size()) - 1);
    return set;
}

QJsonObject writeSessions(SessionSet &set, const QDir &directory,
                          QSet<QString> &referencedImages, bool &imagesSaved)
{
    QJsonArray sessions;
    for (auto &conversation : set.list)
        sessions.append(writeConversation(conversation, directory, referencedImages, imagesSaved));
    return {{"current", set.current}, {"sessions", sessions}};
}

} // namespace

QString ChatHistory::directory(const QString &noteId) const
{
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (data.isEmpty() || noteId.isEmpty()) return {};
    const QString name = QString::fromLatin1(revision(noteId.toUtf8()).toHex());
    return QDir(data).filePath(QStringLiteral("chat-history/") + name);
}

NoteState ChatHistory::load(const QString &noteId) const
{
    NoteState state;
    const auto path = directory(noteId);
    if (path.isEmpty()) return state;
    const QDir folder(path);
    QFile file(folder.filePath(kHistoryFile));
    if (!file.exists()) return state;
    if (!file.open(QIODevice::ReadOnly)) {
        state.historyReadable = false;
        return state;
    }

    const auto data = file.readAll();
    const auto object = QJsonDocument::fromJson(data).object();
    const int version = object.value("version").toInt();
    if (file.error() != QFileDevice::NoError || (version != 1 && version != 2)) {
        state.historyReadable = false;
        return state;
    }
    state.historyRevision = revision(data);
    state.solo = readSessions(object.value("solo").toObject(), folder, version);
    state.council = readSessions(object.value("council").toObject(), folder, version);
    state.contextLimit = object.value("contextLimit").toInteger(-1);
    return state;
}

bool ChatHistory::save(const QString &noteId, NoteState &state, const QList<ChatImage> &activeImages) const
{
    const auto path = directory(noteId);
    if (!state.historyReadable || path.isEmpty() || !QDir().mkpath(path))
        return false;
    const QDir folder(path);
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock()) return false;
    const auto historyPath = folder.filePath(kHistoryFile);
    QFile existing(historyPath);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly) || revision(existing.readAll()) != state.historyRevision
            || existing.error() != QFileDevice::NoError)
            return false;
        existing.close();
    } else if (!state.historyRevision.isEmpty()) {
        return false;
    }

    QSet<QString> referencedImages;
    bool imagesSaved = true;
    const auto solo = writeSessions(state.solo, folder, referencedImages, imagesSaved);
    const auto council = writeSessions(state.council, folder, referencedImages, imagesSaved);
    const QJsonObject object{{"version", 2}, {"solo", solo}, {"council", council},
                             {"contextLimit", state.contextLimit}};
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!writePrivateFile(historyPath, data)) return false;
    state.historyRevision = revision(data);

    for (const auto &image : activeImages)
        referencedImages.insert(image.storedName);
    for (const auto &name : folder.entryList({QStringLiteral("*.jpg")}, QDir::Files | QDir::NoSymLinks)) {
        if (isImageName(name) && !referencedImages.contains(name))
            QFile::remove(folder.filePath(name));
    }
    return imagesSaved;
}

bool ChatHistory::rename(const QString &oldId, const QString &newId) const
{
    const auto oldPath = directory(oldId);
    const auto newPath = directory(newId);
    if (oldPath.isEmpty() || newPath.isEmpty()) return false;
    if (oldPath == newPath || (!QFileInfo::exists(oldPath) && !QFileInfo::exists(newPath))) return true;

    QLockFile oldLock(oldPath + QStringLiteral(".lock"));
    QLockFile newLock(newPath + QStringLiteral(".lock"));
    if (!oldLock.tryLock() || !newLock.tryLock()) return false;

    // A previously deleted note may have left chat history at the destination.
    QDir folder;
    QString backup;
    if (QFileInfo::exists(newPath)) {
        backup = newPath + QStringLiteral(".previous-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!folder.rename(newPath, backup)) return false;
    }
    if (!QFileInfo::exists(oldPath) || folder.rename(oldPath, newPath)) return true;
    if (!backup.isEmpty()) folder.rename(backup, newPath);
    return false;
}

QString ChatHistory::imagePath(const QString &noteId, const ChatImage &image) const
{
    if (image.file) return image.file->fileName();
    return QDir(directory(noteId)).filePath(image.storedName);
}

} // namespace sava
