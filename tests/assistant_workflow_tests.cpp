#include "CodeEditor.hpp"
#include "MainWindow.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("LocalAssistantWorkflow");
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings().setValue("workspace/root", temporary.path());
    QSettings().setValue("lsp/enabled", false);
    QSettings().setValue("assistant/review", false);
    qunsetenv("TS_DISABLE_LOCAL_MODEL");
    try {
        const auto file = temporary.path() + "/add.py";
        QFile source(file);
        if (!source.open(QIODevice::WriteOnly))
            return 1;
        source.write("def add(a, b):\n    return \n");
        source.close();
        MainWindow window;
        window.show();
        auto* editor = window.findChild<CodeEditor*>("codeEditor");
        editor->openFileRequested(file);
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.movePosition(QTextCursor::Left);
        editor->setTextCursor(cursor);
        editor->setFocus();
        QTest::keyClicks(editor, "a");
        QElapsedTimer deadline;
        deadline.start();
        while (editor->property("ghostText").toString().isEmpty() && deadline.elapsed() < 15000)
            QTest::qWait(20);
        const auto suggestion = editor->property("ghostText").toString();
        if (suggestion.isEmpty())
            throw std::runtime_error("Real local model must produce an inline continuation while typing");
        const auto before = editor->toPlainText();
        QTest::keyClick(editor, Qt::Key_Tab);
        if (editor->toPlainText() == before || !editor->toPlainText().contains("b"))
            throw std::runtime_error("Tab must accept the real neural continuation");
        editor->undo();
        if (editor->toPlainText() != before)
            throw std::runtime_error("Neural continuation must undo atomically");
        QTest::keyClicks(editor, "d");
        QTest::keyClick(editor, Qt::Key_Backslash, Qt::AltModifier);
        QTest::qWait(50);
        editor->document()->setModified(false);
        window.findChild<QTabBar*>("documentTabs")->tabCloseRequested(0);
        QTest::qWait(200);
        if (!editor->isReadOnly() || !editor->property("ghostText").toString().isEmpty())
            throw std::runtime_error("Closed documents must reject pending neural results");
        window.close();
        std::cout << "PASS: real in-process model, automatic ghost text, Tab acceptance, Undo and "
                     "closed-document cancellation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
