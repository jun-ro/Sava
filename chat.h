#pragma once

#include "story-context.h"

#include <QString>
#include <QWidget>

#include <functional>
#include <memory>

namespace sava {

enum class Harness;

// One change the assistant asks to make to the open note.
struct DocumentEdit {
    enum class Kind { Append, InsertAfter, Replace };
    Kind kind = Kind::Append;
    QString anchor; // verbatim passage in the note; unused for Append
    QString text;
};

// The co-writing assistant rail. ChatPane owns the conversation for each note,
// drives one agent in Co-write mode or a bounded sequential council, and never
// blocks the UI while an agent is working.
class ChatPane : public QWidget {
public:
    explicit ChatPane(QWidget *parent = nullptr);
    ~ChatPane() override;

    // The pane keeps a separate conversation per note id. Calling this again for
    // the same id refreshes the note text used to build prompts and affordances.
    void setNote(const QString &id, const QString &title, const QString &text);
    void renameNote(const QString &oldId, const QString &newId, const QString &title);
    void openSettings();

    // Set by the parent so assistant messages can insert a suggested line into
    // the active note, and jump to a quoted passage that exists in it.
    std::function<void(const QString &)> insertText;
    std::function<void(const QString &)> jumpToPassage;
    std::function<QString(const QString &noteId, const QString &question, bool lookup)> relatedStory;
    std::function<StoryPage(const QString &noteId, const QString &selector, const QString &cursor)> readStory;

    // Applies an edit to the note with this id as one undo step. Returns an error
    // message when the edit could not be made, or an empty string on success.
    std::function<QString(const QString &noteId, const DocumentEdit &)> editDocument;

    std::function<void(qint64 estimatedTokens, qint64 limit)> contextUpdated;
    std::function<void(Harness)> providerUsageChanged;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sava
