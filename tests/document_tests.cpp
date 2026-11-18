#include "../src/application/DocumentController.hpp"
#include "../src/application/ProjectSearch.hpp"
#include "CodeEditor.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextCursor>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const QString& reason) {
    if (!ok)
        throw std::runtime_error(reason.toStdString());
}

static void edit(EditorDocument* value, const QString& text) {
    QTextCursor cursor(value->text.get());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temp;
        DocumentController docs;
        CodeEditor editor;
        QObject::connect(&docs, &DocumentController::activeChanged, &editor, [&](int i) {
            if (auto* value = docs.document(i))
                editor.useDocument(value->text.get());
        });
        docs.create("int first;", "first.cpp");
        auto* first = docs.activeDocument();
        edit(first, " // unsaved Привет 🔥");
        docs.create("int second;", "second.cpp");
        auto* second = docs.activeDocument();
        edit(second, " // second dirty");
        check(docs.count() == 2 && first->text->isModified() && second->text->isModified(),
              "Independent dirty documents");
        docs.activate(0);
        check(editor.toPlainText().contains("Привет") && docs.activeDocument() == first,
              "Tab activation preserves source");
        QString error;
        const auto file = temp.path() + "/файл с пробелами.cpp";
        check(docs.save(0, file, error), error);
        check(!first->text->isModified(), "Saved flag");
        check(docs.open(file, error) && docs.count() == 2,
              "Opening same canonical path activates existing tab");
        check(!docs.save(1, file, error), "Prevent two buffers overwriting same path");
        const auto renamed = temp.path() + "/переименован.cpp";
        check(docs.renamePath(file, renamed, error), error);
        check(first->path == renamed && QFile::exists(renamed) && !QFile::exists(file),
              "Rename updates open document");
        edit(first, " // dirty again");
        const auto session = temp.path() + "/session.json";
        check(docs.saveSession(session, error), error);
        DocumentController restored;
        check(restored.restoreSession(session, error), error);
        check(restored.count() == 2 &&
                  restored.document(0)->text->toPlainText() == first->text->toPlainText() &&
                  restored.document(1)->text->isModified(),
              "Recover unsaved text and dirty flags");
        int decisions = 0;
        check(!docs.prepareClose(
                  [&](const auto&) {
                      return ++decisions == 1 ? CloseDecision::Discard : CloseDecision::Cancel;
                  },
                  [](const auto&) { return QString(); }, error),
              "Cancel close-all");
        check(first->text->isModified() && second->text->isModified(),
              "Cancelled close must preserve all dirty buffers");
        check(!docs.close(
                  1, [](const auto&) { return CloseDecision::Cancel; }, [](const auto&) { return QString(); },
                  error) &&
                  docs.count() == 2,
              "Cancel tab close");
        check(!docs.close(
                  1, [](const auto&) { return CloseDecision::Save; }, [](const auto&) { return QString(); },
                  error) &&
                  docs.count() == 2,
              "Cancelled save path keeps tab");
        check(docs.close(
                  1, [](const auto&) { return CloseDecision::Discard; },
                  [](const auto&) { return QString(); }, error) &&
                  docs.count() == 1,
              "Explicit discard closes only selected tab");
        QFile corrupt(temp.path() + "/binary.cpp");
        check(corrupt.open(QIODevice::WriteOnly), "Create invalid UTF-8 fixture");
        corrupt.write(QByteArray::fromHex("ff00fe"));
        corrupt.close();
        check(!docs.open(corrupt.fileName(), error), "Reject invalid UTF-8/binary source");
        QDir().mkpath(temp.path() + "/build-ignored");
        QFile ignored(temp.path() + "/build-ignored/ignored.cpp");
        check(ignored.open(QIODevice::WriteOnly), "Create ignored search fixture");
        ignored.write("SEARCH_TOKEN");
        ignored.close();
        QFile searched(temp.path() + "/поиск.cpp");
        check(searched.open(QIODevice::WriteOnly), "Create Unicode search fixture");
        searched.write(QString("строка\n🔥 SEARCH_TOKEN SEARCH_TOKEN\n").toUtf8());
        searched.close();
        auto found = ProjectSearch::scan(temp.path(), "SEARCH_TOKEN", false, {});
        check(found.matches.size() == 2 && found.matches[0].line == 2 && found.matches[0].column == 4,
              "UTF-16 search positions and ignored build directories");
        found = ProjectSearch::scan(temp.path(), "поиск", true, {});
        check(found.matches.size() == 1, "Unicode filename search");
        found = ProjectSearch::scan(temp.path(), "SEARCH_TOKEN", false, [] { return true; });
        check(found.cancelled && found.matches.isEmpty(), "Search cancellation");
        QFile savedSession(session);
        check(savedSession.open(QIODevice::ReadOnly), "Read session fixture");
        auto malformed = QJsonDocument::fromJson(savedSession.readAll()).object();
        auto entries = malformed["documents"].toArray();
        entries.append(entries[0]);
        malformed["documents"] = entries;
        QFile duplicateSession(temp.path() + "/duplicate-session.json");
        check(duplicateSession.open(QIODevice::WriteOnly), "Write duplicate session fixture");
        duplicateSession.write(QJsonDocument(malformed).toJson());
        duplicateSession.close();
        DocumentController duplicateDocs;
        check(!duplicateDocs.restoreSession(duplicateSession.fileName(), error) && duplicateDocs.count() == 0,
              "Duplicate session paths must not restore ambiguous buffers");
        DocumentController clean;
        check(clean.open(searched.fileName(), error), error);
        const auto cleanSession = temp.path() + "/clean-session.json";
        check(clean.saveSession(cleanSession, error), error);
        check(QFile::remove(searched.fileName()), "Remove test-owned file");
        DocumentController missing;
        check(missing.restoreSession(cleanSession, error) && !error.isEmpty(),
              "Missing session files must be reported");
        std::cout << "PASS: multiple documents, editor switching, UTF-8, save/collision/rename, dirty "
                     "session recovery, cancel/discard/close guards\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
