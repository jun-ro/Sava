#pragma once

#include "agents.h"

#include <QByteArray>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QTemporaryFile>

#include <memory>

namespace sava {

inline constexpr int kMaxChatMessages = 40;
inline constexpr int kMaxChatSessions = 30;
inline constexpr int kMaxChatImages = 3;
inline constexpr int kChatImageSide = 1600;
inline constexpr int kChatThumbnailSide = 84;

struct ChatImage {
    std::shared_ptr<QTemporaryFile> file;
    QPixmap thumbnail;
    QString storedName;
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
    bool expanded = false;
    ReplyQuote quote;
    QList<ChatImage> images;
};

struct Conversation {
    QList<Message> messages;
    qint64 lastPromptTokens = -1;
};

// A note's chat sessions for one mode. Never empty; `current` is the open one.
struct SessionSet {
    QList<Conversation> list{Conversation{}};
    int current = 0;

    Conversation &active() { return list[qBound(0, current, int(list.size()) - 1)]; }
    const Conversation &active() const { return list.at(qBound(0, current, int(list.size()) - 1)); }
};

struct NoteState {
    SessionSet solo;
    SessionSet council;
    qint64 contextLimit = -1;
    QByteArray historyRevision;
    bool historyReadable = true;
};

class ChatHistory {
public:
    NoteState load(const QString &noteId) const;
    bool save(const QString &noteId, NoteState &state, const QList<ChatImage> &activeImages) const;
    bool rename(const QString &oldId, const QString &newId) const;
    QString imagePath(const QString &noteId, const ChatImage &image) const;

private:
    QString directory(const QString &noteId) const;
};

} // namespace sava
