#pragma once

#include <QString>
#include <QStringList>

namespace sava {

// Recursively lists note files below root: .md, .markdown and .txt files in
// any nested folder, as sorted absolute paths. Hidden entries and symbolic
// links are skipped, and the files themselves are never opened.
QStringList markdownFiles(const QString &root);

// Reads a note as UTF-8 text. Returns false and clears *text when the file is
// missing, is not a regular file, is a symbolic link, cannot be read, or does
// not contain valid UTF-8; *error then holds a human-readable reason. A UTF-8
// byte order mark is dropped, every other byte is preserved.
bool loadNote(const QString &path, QString *text, QString *error);

// Writes a note as UTF-8 through QSaveFile, replacing the destination
// atomically. The text is validated first so a lossy conversion is never
// written; an existing file that was not valid UTF-8 is only replaced by
// well-formed UTF-8. Symbolic links and non-regular destinations are refused.
bool saveNote(const QString &path, const QString &text, QString *error);

// Creates an empty note in root, using the first free name among Untitled.md,
// Untitled 2.md, Untitled 3.md, ... and reserving it atomically. Returns the
// absolute path of the new file, or an empty string on failure with the reason
// in *error.
QString createNote(const QString &root, QString *error);

} // namespace sava
