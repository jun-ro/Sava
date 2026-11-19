#include "story-context.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTextStream>
#include <algorithm>

namespace sava {
namespace {

constexpr int InitialBytes = 630;
constexpr int LookupBytes = 440;
constexpr qint64 SampleBytes = 10 * 1024;
constexpr int MaxSources = 3;
constexpr int PageBytes = 9 * 1024;
#ifdef Q_OS_WIN
constexpr auto PathCase = Qt::CaseInsensitive;
#else
constexpr auto PathCase = Qt::CaseSensitive;
#endif

bool inside(const QString &file, QString folder)
{
    if (!folder.endsWith('/')) folder += '/';
    return file.startsWith(folder, PathCase);
}

bool storyBranch(const QString &name)
{
    static const QRegularExpression branches(
        R"(^(chapters?|scenes?|manuscript|lore|worldbuilding|characters?|settings?|outlines?|timelines?|references?|notes)$)",
        QRegularExpression::CaseInsensitiveOption);
    return branches.match(name).hasMatch();
}

QString storyScope(const QString &root, const QString &active)
{
    QDir dir = QFileInfo(active).absoluteDir();
    while (inside(dir.absolutePath(), root)) {
        if (storyBranch(dir.dirName()) && dir.cdUp())
            return dir.absolutePath();
        if (!dir.cdUp()) break;
    }
    return QFileInfo(active).absolutePath();
}

bool inStory(const QString &file, const QString &scope, const QString &activeFolder)
{
    if (!inside(file, scope)) return false;
    const QString relative = QDir(scope).relativeFilePath(file);
    return scope == activeFolder || !relative.contains('/')
        || storyBranch(relative.section('/', 0, 0))
        || inside(file, activeFolder);
}

struct Chapter {
    QString kind;
    int number = -1;
};

Chapter chapterNumber(const QString &text)
{
    static const QRegularExpression pattern(
        R"(^(?:(chapter|ch\.?|scene|part)\s*#?\s*([0-9]{1,3})\b|([0-9]{1,3})(?=$|\s|[._–—-])))",
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch()) return {};
    QString kind = match.captured(1).toLower();
    if (kind.isEmpty() || kind.startsWith("ch")) kind = "chapter";
    const QString number = match.captured(2).isEmpty() ? match.captured(3) : match.captured(2);
    return {kind, number.toInt()};
}

struct Note {
    QString path;
    QString title;
    Chapter chapter;
    int score = 0;
    bool linked = false;
    QString found;
};

QStringList references(const QString &text)
{
    static const QRegularExpression link(
        R"re((?<!!)\[\[([^\[\]\r\n]+)\]\]|(?<!!)\[[^\[\]\r\n]+\]\(<?([^<>\s)]+\.(?:md|markdown|txt)(?:#[^<>\s)]*)?)>?\))re",
        QRegularExpression::CaseInsensitiveOption);
    QStringList targets;
    for (const auto &match : link.globalMatch(text)) {
        QString target = match.captured(1);
        if (target.isEmpty()) target = QString::fromUtf8(QByteArray::fromPercentEncoding(match.captured(2).toUtf8()));
        target = target.section('|',0,0).section('#',0,0).trimmed();
        if (!target.isEmpty()) targets.append(target);
    }
    return targets;
}

int resolve(const QList<Note> &notes, const QString &from, const QString &target)
{
    if (QDir::isAbsolutePath(target) || target.contains(':')) return -1;
    const auto directory = QFileInfo(from).absoluteDir();
    const auto relative = QDir::cleanPath(directory.filePath(target));
    int local = -1;
    int unique = -1;
    int count = 0;
    for (int i = 0; i < notes.size(); ++i) {
        const auto &note = notes.at(i);
        const QFileInfo info(note.path);
        if (note.path.compare(relative, PathCase) == 0) return i;
        if (target.contains('/')) {
            if (QDir::cleanPath(info.path() + '/' + note.title).compare(relative, PathCase) == 0)
                return i;
            continue;
        }
        if (note.title.compare(target, Qt::CaseInsensitive) == 0 ||
            info.fileName().compare(target, Qt::CaseInsensitive) == 0) {
            unique = i;
            ++count;
            if (info.path().compare(directory.absolutePath(), PathCase) == 0) local = i;
        }
    }
    return local >= 0 ? local : count == 1 ? unique : -1;
}

bool usableNote(const QString &path)
{
    const QFileInfo info(path);
    return info.isFile() && !info.isSymLink() &&
           info.canonicalFilePath().compare(path, PathCase) == 0;
}

QString sample(const QString &path, bool includeEnding = true)
{
    if (!usableNote(path)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray head = file.read(SampleBytes + 1);
    if (head.size() > SampleBytes) {
        qsizetype end = SampleBytes;
        while (end > 0 && (static_cast<unsigned char>(head.at(end)) & 0xc0) == 0x80) --end;
        head.truncate(end);
    }
    QString text = QString::fromUtf8(head);
    if (includeEnding && file.size() > SampleBytes) {
        const qint64 tail = qMax(SampleBytes, file.size() - SampleBytes);
        if (file.seek(tail)) {
            auto bytes = file.read(SampleBytes);
            qsizetype start = 0;
            while (start < bytes.size() &&
                   (static_cast<unsigned char>(bytes.at(start)) & 0xc0) == 0x80) ++start;
            text += "\n\n" + QString::fromUtf8(bytes.constData() + start, bytes.size() - start);
        }
    }
    return text;
}

QString clip(const QString &text, int limit, bool ending = false)
{
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() <= limit) return text;
    if (limit <= 3) return {};
    if (ending) {
        auto start = bytes.size() - limit + 3;
        while ((static_cast<unsigned char>(bytes.at(start)) & 0xc0) == 0x80) ++start;
        return "…" + QString::fromUtf8(bytes.constData() + start, bytes.size() - start);
    }
    int end = limit - 3;
    while ((static_cast<unsigned char>(bytes.at(end)) & 0xc0) == 0x80) --end;
    return QString::fromUtf8(bytes.constData(), end) + "…";
}

QSet<QString> significantWords(const QString &question)
{
    static const QRegularExpression words(R"([\p{L}\p{N}][\p{L}\p{N}'’-]{2,})");
    static const QSet<QString> common{
        "about", "and", "are", "but", "can", "chapter", "chapters", "could", "current",
        "did", "does", "draft", "expand", "for", "from", "has", "here", "how", "into", "like",
        "make", "next", "not", "note", "previous", "should", "story", "that", "the",
        "their", "them", "there", "these", "this", "those", "universe", "upon", "was",
        "what", "when", "where", "which", "who", "why", "with", "would", "you"
    };
    QSet<QString> result;
    for (const auto &match : words.globalMatch(question.toLower())) {
        const auto word = match.captured();
        if (!common.contains(word)) result.insert(word);
    }
    return result;
}

int overlap(const QString &text, const QSet<QString> &terms)
{
    const auto words = significantWords(text);
    int score = 0;
    for (const auto &term : terms) score += words.contains(term);
    return score;
}

QString findPassage(const QString &path, const QSet<QString> &terms, qint64 &budget)
{
    if (!usableNote(path) || terms.isEmpty() || budget <= 0) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QTextStream reader(&file);
    QString best;
    int bestScore = 0;
    while (!reader.atEnd() && budget > 0) {
        const auto raw = reader.readLine(600);
        budget -= raw.toUtf8().size() + 1;
        const auto line = raw.simplified();
        const int score = overlap(line, terms);
        if (score <= bestScore) continue;
        best = line;
        bestScore = score;
        if (score == terms.size()) break;
    }
    qsizetype firstMatch = best.size();
    for (const auto &term : terms) {
        const auto position = best.indexOf(term, 0, Qt::CaseInsensitive);
        if (position >= 0) firstMatch = qMin(firstMatch, position);
    }
    if (firstMatch > 60 && firstMatch < best.size()) {
        const auto boundary = best.lastIndexOf(' ', firstMatch - 60);
        best = "…" + best.sliced(boundary >= 0 ? boundary + 1 : firstMatch - 60);
    }
    return best;
}

QString excerpt(const QString &text, const QSet<QString> &terms, int limit, bool ending)
{
    if (ending) return clip(text.simplified(), limit, true);
    static const QRegularExpression paragraphs(R"(\r?\n\s*\r?\n)");
    static const QRegularExpression headings(R"(^[ \t]*#{1,6}[ \t]+[^\r\n]*\r?\n?)",
                                             QRegularExpression::MultilineOption);
    QString best;
    int bestScore = -1;
    for (const auto &block : text.split(paragraphs, Qt::SkipEmptyParts)) {
        const auto content = QString(block).remove(headings).simplified();
        if (content.isEmpty() || content == "---") continue;
        const int score = overlap(content, terms);
        if (score > bestScore) {
            best = content;
            bestScore = score;
        }
    }
    return clip(best.isEmpty() ? text.simplified() : best, limit);
}

struct TextSlice {
    QString text;
    qsizetype bytes = 0;
    bool more = false;
    bool valid = false;
};

TextSlice readSlice(const QString &path, const QString &active, const QString &activeText,
                    qint64 offset, int limit)
{
    QByteArray bytes;
    if (path == active) {
        bytes = activeText.toUtf8();
        if (offset > bytes.size()) return {};
        bytes = bytes.mid(offset, limit + 1);
    } else {
        if (!usableNote(path)) return {};
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || offset > file.size() || !file.seek(offset)) return {};
        bytes = file.read(limit + 1);
        if (file.error() != QFileDevice::NoError) return {};
    }
    qsizetype length = bytes.size();
    const bool more = length > limit;
    if (more) {
        length = limit;
        while (length > 0 && (static_cast<unsigned char>(bytes.at(length)) & 0xc0) == 0x80)
            --length;
    }
    const auto flags = offset == 0 ? QStringDecoder::Flag::Stateless
        : QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom;
    QStringDecoder utf8(QStringDecoder::Utf8, flags);
    const QString text = utf8(QByteArrayView(bytes).first(length));
    if (utf8.hasError() || (more && length == 0)) return {};
    return {text, length, more, true};
}

} // namespace

QString storyContext(const QString &root, const QStringList &files, const QString &active,
                     const QString &activeText, const QString &question, bool lookup)
{
    if (root.isEmpty() || active.isEmpty() || question.trimmed().isEmpty()) return {};
    static const QRegularExpression asksPrevious(
        R"(\b(previous|last|preceding|prior)\s+(chapter|scene|part)s?\b|\b(continue|pick up)\s+(from|where)\b)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression asksNext(R"(\b(next|following)\s+(chapter|scene|part)s?\b)",
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression storyWords(
        R"(\b(chapters?|scenes?|story|universe|canon|lore|worldbuilding|continuity|characters?|timeline|plot|sequel|setting|outline|expand|continue|foreshadow\w*)\b)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression storyMeta(
        R"(\b(bible|worldbuilding|lore|characters?|timeline|settings?|outline|synopsis|canon)\b)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression outlineName(
        R"(^(outline|index|story map|table of contents|toc|chapter list)$)",
        QRegularExpression::CaseInsensitiveOption);
    const bool previousRequested = asksPrevious.match(question).hasMatch();
    const bool nextRequested = asksNext.match(question).hasMatch();
    const bool narrative = previousRequested || nextRequested || storyWords.match(question).hasMatch();
    const QString target = question.section('|',0,0).trimmed();
    const auto terms = significantWords(lookup && question.contains('|')
        ? question.section('|',1).trimmed() : question);
    const QFileInfo activeInfo(active);
    const QString folder = activeInfo.absolutePath();
    const QString scope = storyScope(root, active);
    const QDir storyDirectory(scope);

    QList<Note> notes;
    int current = -1;
    QStringList sorted = files;
    sorted.sort(PathCase);
    for (const auto &file : sorted) {
        if (!inside(file, root)) continue;
        const QString title = QFileInfo(file).completeBaseName();
        if (file == active) current = notes.size();
        notes.append({file,title,chapterNumber(title),0,false,{}});
    }
    if (current < 0) return {};
    int exact = lookup ? resolve(notes,active,target) : -1;
    if (lookup && exact < 0 && target.contains('/'))
        exact = resolve(notes,storyDirectory.filePath("outline.md"),target);

    auto chapter = notes.at(current).chapter;
    if (chapter.number < 0) {
        static const QRegularExpression heading(R"(^\s*# \s*([^\r\n]+))");
        chapter = chapterNumber(heading.match(activeText.left(400)).captured(1));
    }

    int previous = -1;
    int next = -1;
    if (narrative && chapter.number >= 0) {
        for (int i = 0; i < notes.size(); ++i) {
            const auto &note = notes.at(i);
            if (QFileInfo(note.path).absolutePath().compare(folder, PathCase) != 0 ||
                note.chapter.kind != chapter.kind || note.chapter.number < 0) continue;
            const int n = note.chapter.number;
            if (n < chapter.number && (previous < 0 || notes.at(previous).chapter.number < n)) previous = i;
            if (n > chapter.number && (next < 0 || notes.at(next).chapter.number > n)) next = i;
        }
    }
    // An outline's link order can supply adjacency when chapter files have names instead of numbers.
    if ((previousRequested && previous < 0) || (nextRequested && next < 0)) {
        int checked = 0;
        for (const auto &outline : notes) {
            if (!inStory(outline.path, scope, folder) || !outlineName.match(outline.title).hasMatch()) continue;
            if (++checked > 3) break;
            QList<int> ordered;
            for (const auto &link : references(sample(outline.path,false))) {
                const int id = resolve(notes,outline.path,link);
                if (id >= 0 && !ordered.contains(id)) ordered.append(id);
            }
            const auto position = ordered.indexOf(current);
            if (position < 0) continue;
            if (previous < 0 && position > 0) previous = ordered.at(position - 1);
            if (next < 0 && position + 1 < ordered.size()) next = ordered.at(position + 1);
            break;
        }
    }

    for (const auto &link : references(activeText.left(24 * 1024))) {
        const int id = resolve(notes,active,link);
        if (id >= 0) notes[id].linked = true;
    }
    for (int i = 0; i < notes.size(); ++i) {
        auto &note = notes[i];
        if (i == exact) { note.score += 160; continue; }
        if (i == current || (exact >= 0 && lookup) ||
            (!note.linked && !inStory(note.path, scope, folder))) continue;
        const int words = overlap(note.title, terms);
        if (words > 0)
            note.score += 18 + qMin(words,4) * 9;
        if (!lookup && narrative && note.linked) note.score += 32;
        if (!lookup && narrative && !previousRequested && !nextRequested &&
            storyMeta.match(note.title).hasMatch()) note.score += 12;
        if (i == previous && (previousRequested || (!lookup && narrative && !nextRequested)))
            note.score += previousRequested ? 100 : 22;
        if (i == next && nextRequested) note.score += 100;
    }

    if (lookup && !terms.isEmpty() && !previousRequested && !nextRequested &&
        (exact < 0 || question.contains('|'))) {
        qint64 searchBudget = exact >= 0 ? 512 * 1024 : 4 * 1024 * 1024;
        int searched = 0;
        for (int i = 0; i < notes.size() && searchBudget > 0; ++i) {
            auto &note = notes[i];
            if (exact >= 0 ? i != exact : i == current ||
                (!note.linked && !inStory(note.path, scope, folder))) continue;
            if (++searched > 64) break;
            const auto initial = qMin<qint64>(searchBudget, exact >= 0 ? 512 * 1024 : 64 * 1024);
            qint64 budget = initial;
            if (i == current) {
                const bool match = std::any_of(terms.cbegin(), terms.cend(), [&](const QString &term) {
                    return activeText.contains(term, Qt::CaseInsensitive);
                });
                if (match) note.found = excerpt(activeText,terms,LookupBytes,false);
            } else {
                note.found = findPassage(note.path, terms, budget);
            }
            searchBudget -= initial - budget;
            if (!note.found.isEmpty()) note.score += 35 + 14 * overlap(note.found, terms);
        }
        if (exact >= 0 && notes.at(exact).found.isEmpty()) return {};
    }

    QList<int> ranked;
    for (int i = 0; i < notes.size(); ++i)
        if (notes.at(i).score > 0) ranked.append(i);
    std::stable_sort(ranked.begin(), ranked.end(), [&notes](int a, int b) {
        return notes.at(a).score > notes.at(b).score;
    });
    QString result;
    int remaining = lookup ? LookupBytes : InitialBytes;
    int sources = 0;
    int reads = 0;
    for (int id : ranked) {
        if (sources == (lookup ? 1 : MaxSources) || remaining < 100 || ++reads > 8) break;
        const auto &note = notes.at(id);
        const bool targetedPassage = !note.found.isEmpty();
        const QString text = !note.found.isEmpty() ? note.found
            : id == current ? activeText : sample(note.path);
        if (text.trimmed().isEmpty()) continue;
        const bool ending = id == previous;
        const QString unit = chapter.kind.isEmpty() ? QString("chapter") : chapter.kind;
        const QString relation = ending ? QString("Previous %1 ending").arg(unit)
            : id == next && nextRequested ? QString("Next %1").arg(unit)
            : targetedPassage ? "Matching passage" : note.linked ? "Linked note" : "Related note";
        const QString label = "- " + relation + " · "
            + clip(storyDirectory.relativeFilePath(note.path).simplified(), 140) + ": ";
        const int available = qMin(remaining - int(label.toUtf8().size()) - 1,
                                   lookup ? LookupBytes : sources == 0 ? 400 : 300);
        const auto body = excerpt(text,terms,available,ending);
        if (body.isEmpty()) continue;
        const auto line = label + body + '\n';
        remaining -= line.toUtf8().size();
        result += line;
        ++sources;
    }
    return sources ? result : QString();
}

StoryPage readStoryPage(const QString &root, const QStringList &files, const QString &active,
                        const QString &activeText, const QString &selector, const QString &cursor)
{
    if (root.isEmpty() || !inside(active, root)) return {};
    const QString scope = storyScope(root, active);
    const QDir story(scope);
    const QString folder = QFileInfo(active).absolutePath();
    const bool wholeStory = selector == QStringLiteral("@story");
    const bool index = selector == QStringLiteral("@index");
    QStringList selected;
    if (selector == QStringLiteral("@active")) {
        selected.append(active);
    } else {
        QList<Note> notes;
        for (const auto &file : files) {
            if (inside(file, root))
                notes.append({file, QFileInfo(file).completeBaseName(), {}, 0, false, {}});
        }
        if (wholeStory || index) {
            QSet<QString> linked;
            for (const auto &link : references(activeText.left(24 * 1024))) {
                const int id = resolve(notes, active, link);
                if (id >= 0) linked.insert(notes.at(id).path);
            }
            for (const auto &note : notes) {
                if (note.path == active || inStory(note.path, scope, folder) || linked.contains(note.path))
                    selected.append(note.path);
            }
            selected.removeDuplicates();
            QCollator order(QLocale(QLocale::English));
            order.setNumericMode(true);
            std::sort(selected.begin(), selected.end(), [&order](const QString &a, const QString &b) {
                const auto comparison = order.compare(a, b);
                return comparison == 0 ? a < b : comparison < 0;
            });
        } else {
            int id = resolve(notes, story.filePath("outline.md"), selector);
            if (id < 0) id = resolve(notes, active, selector);
            if (id < 0) id = resolve(notes, QDir(root).filePath("outline.md"), selector);
            if (id >= 0) selected.append(notes.at(id).path);
        }
    }
    if (selected.isEmpty()) return {};

    qint64 first = 0;
    qint64 offset = 0;
    if (!cursor.isEmpty()) {
        bool ok = false;
        if (wholeStory) {
            first = cursor.section(':', 0, 0).toLongLong(&ok);
            if (!ok) return {};
            offset = cursor.section(':', 1).toLongLong(&ok);
        } else if (index) {
            first = cursor.toLongLong(&ok);
        } else {
            offset = cursor.toLongLong(&ok);
        }
        if (!ok || first < 0 || first >= selected.size() || offset < 0) return {};
    }

    StoryPage page;
    const auto nextAt = [&](qint64 note, qint64 byte) {
        if (note >= selected.size()) return QString();
        if (wholeStory) return QStringLiteral("%1:%2").arg(note).arg(byte);
        return QString::number(index ? note : byte);
    };
    int remaining = index ? LookupBytes * 3 : PageBytes;
    for (qint64 i = first; i < selected.size(); ++i, offset = 0) {
        const auto &path = selected.at(i);
        const auto name = story.relativeFilePath(path).replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QString header = index ? "- " + name + '\n'
            : QStringLiteral("### %1 (byte %2)\n").arg(name).arg(offset);
        const int overhead = header.toUtf8().size() + (index ? 0 : 2);
        if (!page.text.isEmpty() && remaining < overhead + (index ? 0 : 256)) {
            page.next = nextAt(i, offset);
            break;
        }
        if (index) {
            page.text += header;
            remaining -= overhead;
            continue;
        }
        const auto slice = readSlice(path, active, activeText, offset, qMax(256, remaining - overhead));
        if (!slice.valid) {
            const QString issue = QStringLiteral("[Could not read this note or byte position.]\n\n");
            page.text += header + issue;
            remaining -= overhead + issue.toUtf8().size();
            continue;
        }
        page.text += header + slice.text + QStringLiteral("\n\n");
        remaining -= overhead + slice.bytes;
        if (slice.more) {
            page.next = nextAt(i, offset + slice.bytes);
            break;
        }
    }
    return page;
}

} // namespace sava
