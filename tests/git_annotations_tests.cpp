#include "CodeEditor.hpp"
#include "GitAnnotations.hpp"
#include "GitPanel.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

static void git(const QString& directory, const QStringList& arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start("git", QStringList{"-c", "core.hooksPath=" + directory + "/no-hooks"} + arguments);
    check(process.waitForFinished(10000) && process.exitCode() == 0, "Git fixture command failed");
}

static bool waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000)
        QTest::qWait(20);
    return predicate();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("GitAnnotations");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    try {
        git(directory.path(), {"init", "--quiet"});
        git(directory.path(), {"config", "user.name", "First Author"});
        git(directory.path(), {"config", "user.email", "first@example.test"});
        git(directory.path(), {"config", "commit.gpgsign", "false"});
        const auto path = directory.filePath(QString::fromUtf8("source with spaces 文.cpp"));
        const auto write = [&](const QByteArray& text) {
            QFile file(path);
            check(file.open(QIODevice::WriteOnly), "Write blame fixture");
            check(file.write(text) == text.size(), "Write all blame contents");
        };
        write("int first = 1;\nint second = 2;\nint third = 3;\n");
        git(directory.path(), {"add", "--all"});
        git(directory.path(), {"commit", "--quiet", "-m", "initial lines"});
        git(directory.path(), {"config", "user.name", "Second Author"});
        write("int first = 1;\nint second = 20;\nint third = 3;\n");
        git(directory.path(), {"commit", "--quiet", "-am", "update second"});
        CodeEditor editor;
        editor.setPlainText("int first = 1;\nint second = 20;\nint third = 3;\n");
        editor.setFilePath(path);
        editor.setAnnotationsVisible(true);
        auto* annotations = editor.annotations();
        check(waitFor([&] { return annotations->lines().size() >= 3; }),
              "Asynchronous Git annotations arrive");
        check(annotations->lines()[0].author == "First Author" &&
                  annotations->lines()[1].author == "Second Author" &&
                  annotations->lines()[2].author == "First Author",
              "Each line shows its actual author");
        check(annotations->lines()[1].summary == "update second" && annotations->lines()[1].timestamp > 0,
              "Annotations include commit details and time");
        editor.resize(960, 400);
        editor.show();
        editor.jumpToLine(2);
        editor.toggleBookmark(2);
        app.processEvents();
        QSignalSpy inspected(&editor, &CodeEditor::commitInspectionRequested);
        auto* gutter = editor.findChild<QWidget*>("editorGutter");
        QTest::mouseClick(gutter, Qt::LeftButton, Qt::NoModifier,
                          QPoint(8, editor.cursorRect().center().y()));
        check(inspected.size() == 1 && inspected.first().first().toString() == annotations->lines()[1].commit,
              "Clicking an annotation requests the corresponding commit");
        GitPanel panel(nullptr, [](const QString&, int) {});
        panel.setProject(directory.path());
        check(waitFor([&] { return panel.hasRepository(); }), "Git tool window finds the repository");
        panel.showCommitDetails(annotations->lines()[1].commit);
        check(waitFor([&] {
                  return panel.findChild<QPlainTextEdit*>("gitDiff")->toPlainText().contains("update second");
              }),
              "Selected commit opens with its details and diff");
        QTemporaryDir unrelated;
        git(unrelated.path(), {"init", "--quiet"});
        panel.setProject(unrelated.path());
        check(waitFor([&] { return panel.hasRepository(); }), "Switch Git tool window to another repository");
        panel.showCommitDetails(annotations->lines()[1].commit, path);
        check(waitFor([&] {
                  return panel.findChild<QPlainTextEdit*>("gitDiff")->toPlainText().contains("update second");
              }),
              "Annotations inspect the file's repository even when it differs from the workspace");
        if (qEnvironmentVariableIsSet("TS_GUTTER_PREVIEW"))
            editor.grab().save(qEnvironmentVariable("TS_GUTTER_PREVIEW"));
        auto cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.insertText("// unsaved\n");
        check(annotations->lines().isEmpty(), "Editing immediately removes stale line authors");
        check(waitFor([&] { return annotations->lines().size() >= 4; }), "Unsaved buffer is re-annotated");
        check(annotations->lines()[0].commit == QString(40, '0') &&
                  annotations->lines()[1].author == "First Author" &&
                  annotations->lines()[2].author == "Second Author",
              "Unsaved insertions keep committed authors aligned");
        editor.setFilePath(directory.filePath("untracked.cpp"));
        check(waitFor([&] { return !annotations->error().isEmpty(); }),
              "Untracked files give an explicit unavailable state");
        check(annotations->lines().isEmpty(), "An earlier file cannot leak authors into another file");
        editor.setAnnotationsVisible(false);
        check(!annotations->enabled() && annotations->lines().isEmpty(),
              "Hiding annotations cancels pending work");

        const QByteArray hash(64, 'a');
        const auto parsed = GitAnnotations::parse(
            hash + " 1 2 1\nauthor Unicode Author\nauthor-time 123\nsummary title\nfilename x\n" + hash +
                " 1 1 1\nfilename x\n",
            3);
        check(parsed[0].author == "Unicode Author" && parsed[1].commit.size() == 64 &&
                  parsed[2].commit.isEmpty(),
              "Incremental parser supports SHA-256, cached commit metadata and out-of-order ranges");
        std::cout << "PASS: real Git line authors, unsaved contents and stale-request rejection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
