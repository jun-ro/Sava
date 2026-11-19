#include "workspace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringConverter>

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

void clearError(QString *error)
{
    if (error)
        error->clear();
}

QString quotedPath(const QString &path)
{
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(path));
}

bool hasNoteSuffix(const QString &fileName)
{
    const QString lower = fileName.toLower();
    return lower.endsWith(QLatin1String(".md"))
            || lower.endsWith(QLatin1String(".markdown"))
            || lower.endsWith(QLatin1String(".txt"));
}

// QDir::NoSymLinks keeps symbolic links out of the listing, so a link can
// neither add a note nor make the walk recurse into another directory.
void collectNoteFiles(const QString &directory, QStringList *files)
{
    const QDir dir(directory);
    const QFileInfoList entries = dir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
            QDir::NoSort);

    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) {
            collectNoteFiles(entry.absoluteFilePath(), files);
        } else if (entry.isFile() && hasNoteSuffix(entry.fileName())) {
            files->append(entry.absoluteFilePath());
        }
    }
}

bool decodeUtf8(const QByteArray &bytes, QString *text)
{
    QStringDecoder decoder(QStringConverter::Utf8);
    QString decoded = decoder.decode(bytes);
    if (decoder.hasError())
        return false;

    if (decoded.startsWith(QChar(0xFEFF)))
        decoded.remove(0, 1);

    *text = decoded;
    return true;
}

} // namespace

namespace sava {

QStringList markdownFiles(const QString &root)
{
    QStringList files;
    if (root.isEmpty())
        return files;

    const QFileInfo rootInfo(root);
    if (!rootInfo.exists() || !rootInfo.isDir())
        return files;

    const QString rootPath = rootInfo.canonicalFilePath().isEmpty()
            ? rootInfo.absoluteFilePath()
            : rootInfo.canonicalFilePath();
    collectNoteFiles(rootPath, &files);
#ifdef Q_OS_WIN
    files.sort(Qt::CaseInsensitive);
#else
    files.sort(Qt::CaseSensitive);
#endif
    return files;
}

bool loadNote(const QString &path, QString *text, QString *error)
{
    clearError(error);
    if (text)
        text->clear();

    if (path.isEmpty()) {
        setError(error, QStringLiteral("No note path was given."));
        return false;
    }

    const QFileInfo info(path);
    if (info.isSymLink()) {
        setError(error, QStringLiteral("Cannot open %1: it is a symbolic link.")
                        .arg(quotedPath(path)));
        return false;
    }
    if (!info.exists()) {
        setError(error, QStringLiteral("Cannot open %1: it does not exist.")
                        .arg(quotedPath(path)));
        return false;
    }
    if (!info.isFile()) {
        setError(error, QStringLiteral("Cannot open %1: it is not a regular file.")
                        .arg(quotedPath(path)));
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot open %1: %2")
                        .arg(quotedPath(path), file.errorString()));
        return false;
    }

    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        setError(error, QStringLiteral("Cannot read %1: %2")
                        .arg(quotedPath(path), file.errorString()));
        return false;
    }

    QString decoded;
    if (!decodeUtf8(bytes, &decoded)) {
        setError(error, QStringLiteral("Cannot open %1: the file is not valid UTF-8.")
                        .arg(quotedPath(path)));
        return false;
    }

    if (text)
        *text = decoded;
    return true;
}

bool saveNote(const QString &path, const QString &text, QString *error)
{
    clearError(error);

    if (path.isEmpty()) {
        setError(error, QStringLiteral("No note path was given."));
        return false;
    }

    // A lone surrogate cannot survive the trip to UTF-8; toUtf8() would
    // silently substitute U+FFFD, so validate the round trip instead.
    const QByteArray bytes = text.toUtf8();
    if (QString::fromUtf8(bytes) != text) {
        setError(error, QStringLiteral("Cannot save %1: the text contains characters "
                                       "that are not valid Unicode.").arg(quotedPath(path)));
        return false;
    }

    const QFileInfo target(path);
    if (target.isSymLink()) {
        setError(error, QStringLiteral("Cannot save %1: it is a symbolic link.")
                        .arg(quotedPath(path)));
        return false;
    }
    if (target.exists() && !target.isFile()) {
        setError(error, QStringLiteral("Cannot save %1: it is not a regular file.")
                        .arg(quotedPath(path)));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Cannot save %1: %2")
                        .arg(quotedPath(path), file.errorString()));
        return false;
    }

    if (file.write(bytes) != bytes.size()) {
        const QString reason = file.errorString();
        file.cancelWriting();
        setError(error, QStringLiteral("Cannot save %1: %2")
                        .arg(quotedPath(path), reason));
        return false;
    }

    if (!file.commit()) {
        setError(error, QStringLiteral("Cannot save %1: %2")
                        .arg(quotedPath(path), file.errorString()));
        return false;
    }

    return true;
}

QString createNote(const QString &root, QString *error)
{
    clearError(error);

    if (root.isEmpty()) {
        setError(error, QStringLiteral("No note folder was given."));
        return QString();
    }

    const QFileInfo rootInfo(root);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        setError(error, QStringLiteral("Cannot create a note: %1 is not a folder.")
                        .arg(quotedPath(root)));
        return QString();
    }

    const QDir dir(rootInfo.absoluteFilePath());
    for (int index = 1; index <= 10000; ++index) {
        const QString name = index == 1
                ? QStringLiteral("Untitled.md")
                : QStringLiteral("Untitled %1.md").arg(index);
        const QString path = dir.absoluteFilePath(name);

        // NewOnly maps to O_CREAT | O_EXCL, so two callers can never reserve
        // the same name even when they race.
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            file.close();
            return path;
        }

        // A failed open means the name is taken only if the file now exists;
        // any other failure (missing permissions, full disk) will not improve
        // by trying the next name.
        if (!QFileInfo::exists(path)) {
            setError(error, QStringLiteral("Cannot create a note in %1: %2")
                            .arg(quotedPath(root), file.errorString()));
            return QString();
        }
    }

    setError(error, QStringLiteral("Cannot create a note in %1: too many Untitled notes exist.")
                    .arg(quotedPath(root)));
    return QString();
}

} // namespace sava
