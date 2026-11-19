#pragma once

#include <QString>
#include <QStringList>

namespace sava {

// Selects a compact, grounded story context from the active workspace.
QString storyContext(const QString &root, const QStringList &files, const QString &active,
                     const QString &activeText, const QString &question, bool lookup = false);

struct StoryPage {
    QString text;
    QString next;
};

// Reads verbatim text in bounded pages. @active, @story and @index are special
// selectors. All other selectors are note paths relative to the story or root.
StoryPage readStoryPage(const QString &root, const QStringList &files, const QString &active,
                        const QString &activeText, const QString &selector, const QString &cursor);

} // namespace sava
