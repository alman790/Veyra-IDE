#include "CodeEditor.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char* reason) {
    if (!ok)
        throw std::runtime_error(reason);
}

static void key(CodeEditor& editor, int value, const QString& text = {},
                Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, value, modifiers, text);
    QApplication::sendEvent(&editor, &event);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTest");
    app.setApplicationName("EditorRegression");
    QTemporaryDir preferences;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    try {
        CodeEditor editor;
        editor.show();
        editor.activateWindow();
        editor.raise();
        app.processEvents();
        editor.setFocus();
        key(editor, Qt::Key_BraceLeft, "{");
        check(editor.toPlainText() == "{}" && editor.textCursor().position() == 1, "Paired braces");
        key(editor, Qt::Key_Return, "\r");
        check(editor.toPlainText() == "{\n    \n}" && editor.textCursor().blockNumber() == 1,
              "Newline indentation inside brace pair");
        editor.undo();
        check(editor.toPlainText() == "{}", "Indentation is one undo group");
        key(editor, Qt::Key_Backspace);
        check(editor.toPlainText().isEmpty(), "Delete paired brackets");
        key(editor, Qt::Key_ParenLeft, "(");
        key(editor, Qt::Key_ParenRight, ")");
        check(editor.toPlainText() == "()" && editor.textCursor().position() == 2,
              "Skip existing closing bracket");
        editor.setPlainText("");
        key(editor, Qt::Key_QuoteDbl, "\"");
        check(editor.toPlainText() == "\"\"" && editor.textCursor().position() == 1, "Pair double quotes");
        key(editor, Qt::Key_QuoteDbl, "\"");
        check(editor.toPlainText() == "\"\"" && editor.textCursor().position() == 2, "Skip existing quote");
        key(editor, Qt::Key_Left);
        key(editor, Qt::Key_Backspace);
        check(editor.toPlainText().isEmpty(), "Delete quote pair together");
        key(editor, Qt::Key_Apostrophe, "'");
        check(editor.toPlainText() == "''", "Pair single quotes");
        editor.setLanguage(ts::LanguageId::Python);
        editor.setPlainText("f");
        auto prefixCursor = editor.textCursor();
        prefixCursor.movePosition(QTextCursor::End);
        editor.setTextCursor(prefixCursor);
        key(editor, Qt::Key_QuoteDbl, "\"");
        check(editor.toPlainText() == "f\"\"", "String-prefix quote pairing comes from the language adapter");
        editor.setLanguage(ts::LanguageId::Cpp);
        editor.setPlainText("one\ntwo");
        auto cursor = editor.textCursor();
        cursor.select(QTextCursor::Document);
        editor.setTextCursor(cursor);
        key(editor, Qt::Key_Tab, "\t");
        check(editor.toPlainText() == "    one\n    two", "Indent selected lines");
        cursor = editor.textCursor();
        cursor.select(QTextCursor::Document);
        editor.setTextCursor(cursor);
        key(editor, Qt::Key_Backtab, {}, Qt::ShiftModifier);
        check(editor.toPlainText() == "one\ntwo", "Unindent selected lines");
        editor.setPlainText("/*🔥*/ (x)");
        cursor = editor.textCursor();
        cursor.setPosition(8);
        editor.setTextCursor(cursor);
        app.processEvents();
        check(editor.extraSelections().size() >= 3,
              "Bracket matching uses UTF-16 positions after Unicode comment");
        editor.setPlainText("plain");
        cursor = editor.textCursor();
        cursor.select(QTextCursor::WordUnderCursor);
        editor.setTextCursor(cursor);
        key(editor, Qt::Key_BracketLeft, "[");
        check(editor.toPlainText() == "[plain]", "Wrap selected text");
        editor.setPlainText("alphaFirst alphaSecond\nal");
        cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        app.processEvents();
        QTest::keyClick(&editor, Qt::Key_Space, Qt::ControlModifier);
        auto* completion = editor.findChild<QCompleter*>();
        check(completion && completion->popup()->isVisible(), "Completion below the current cursor");
        int sourceChanges = 0;
        QObject::connect(&editor, &CodeEditor::sourceTextChanged, &editor, [&] { ++sourceChanges; });
        editor.setSemanticTokens(QJsonArray{0, 0, 10, 0, 0}, {"variable"});
        check(sourceChanges == 0 && completion->popup()->isVisible(),
              "Semantic highlighting preserves live completion without reporting source edits");
        const int position = editor.textCursor().position();

        QTest::keyClick(&editor, Qt::Key_Down);
        const auto selected = completion->popup()->currentIndex().data().toString();
        check(editor.textCursor().position() == position, "Popup arrows preserve caret");
        editor.setCompletionWords({"alphaFirst", "alphaSecond"});
        check(completion->popup()->currentIndex().data().toString() == selected,
              "Server updates preserve popup selection");
        QTest::keyClick(&editor, Qt::Key_Escape);
        editor.setCompletionWords({"alphaFirst", "alphaSecond"});
        check(!completion->popup()->isVisible(), "Late completion cannot reopen dismissed popup");
        QTest::keyClick(&editor, Qt::Key_Space, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_Return);
        check(editor.toPlainText().endsWith("alphaFirst") || editor.toPlainText().endsWith("alphaSecond"),
              "Enter inserts selected completion");
        editor.setPlainText("fooOne");
        cursor = editor.textCursor();
        cursor.setPosition(2);
        editor.setTextCursor(cursor);
        QTest::keyClick(&editor, Qt::Key_Space, Qt::ControlModifier);
        editor.setCompletionWords({"fooOne"});
        QTest::keyClick(&editor, Qt::Key_Return);
        check(editor.toPlainText() == "fooOne", "Mid-word completion replaces the suffix too");
        editor.setPlainText("Wid");
        cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        QTest::keyClick(&editor, Qt::Key_Space, Qt::ControlModifier);
        const QJsonObject editStart{{"line", 0}, {"character", 0}}, editEnd{{"line", 0}, {"character", 3}};
        const QJsonObject extra{{"range", QJsonObject{{"start", editStart}, {"end", editStart}}},
                                {"newText", "#include <widget>\n"}};
        editor.setCompletionWords(
            {"Widget"},
            {{"Widget", QJsonObject{{"range", QJsonObject{{"start", editStart}, {"end", editEnd}}},
                                    {"additionalTextEdits", QJsonArray{extra}}}}});
        QTest::keyClick(&editor, Qt::Key_Return);
        check(editor.toPlainText() == "#include <widget>\nWidget",
              "SDK completion applies exact ranges and imports together");
        editor.undo();
        check(editor.toPlainText() == "Wid", "Completion and import undo atomically");
        int requests = 0;
        editor.setCompletionRequest([&] { ++requests; });
        editor.setPlainText("alphaFirst\n");
        cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        QTest::keyClicks(&editor, "al");
        QTest::keyClick(&editor, Qt::Key_Left);
        QTest::qWait(300);
        check(requests == 0 && !completion->popup()->isVisible(),
              "Navigation cancels pending completion timer");
        editor.setPlainText("alphaFirst\n");
        cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);
        QTest::keyClicks(&editor, "al");
        QTest::qWait(300);
        check(requests > 0 && completion->popup()->isVisible(),
              "Suggestions appear automatically while typing");
        QTimer::singleShot(30, &editor,
                           [&] { editor.setSemanticTokens(QJsonArray{0, 0, 10, 0, 0}, {"variable"}); });
        QTest::qWait(100);
        check(completion->popup()->isVisible(),
              "Delayed semantic responses do not dismiss the live completion popup");
        QTest::keyClick(&editor, Qt::Key_Escape);
        QString large;
        for (int line = 0; line < 12000; ++line)
            large += QString("int value_%1 = %1;\n").arg(line);
        editor.setPlainText(large);
        editor.jumpToLine(10000);
        const auto start = editor.textCursor().blockNumber();
        QElapsedTimer navigation;
        navigation.start();
        for (int step = 0; step < 300; ++step)
            QTest::keyClick(&editor, Qt::Key_Up);
        check(editor.textCursor().blockNumber() == start - 300, "Every Up press moves in a 12000-line file");
        for (int step = 0; step < 300; ++step)
            QTest::keyClick(&editor, Qt::Key_Down);
        check(editor.textCursor().blockNumber() == start, "Every Down press moves in a 12000-line file");
        check(navigation.elapsed() < 5000, "Large-file navigation remains responsive");
        std::cout << "PASS: editor pairing, matching, indentation, selection wrapping and undo\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
