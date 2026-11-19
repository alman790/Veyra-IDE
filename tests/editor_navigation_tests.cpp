#include "../src/application/DocumentController.hpp"
#include "CodeEditor.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QElapsedTimer>
#include <QFile>
#include <QInputMethodEvent>
#include <QPlainTextDocumentLayout>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextLayout>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("EditorNavigation");
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    try {
        QString large;
        large.reserve(4000000);
        for (int line = 0; line < 100000; ++line)
            large += QString("int variable_%1 = %1;\n").arg(line);
        QFile file(temporary.filePath("large.cpp"));
        check(file.open(QIODevice::WriteOnly), "Create large document");
        file.write(large.toUtf8());
        file.close();
        DocumentController documents;
        QString error;
        check(documents.open(file.fileName(), error),
              "Open a file larger than 2 MiB through the document controller");
        CodeEditor editor;
        editor.resize(1000, 700);
        editor.show();
        editor.useDocument(documents.activeDocument()->text.get());
        editor.setFocus();
        app.processEvents();
        editor.jumpToLine(90000);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 500; ++i)
            QTest::keyClick(&editor, Qt::Key_Up);
        check(editor.textCursor().blockNumber() == 89499, "No lost Up events in a 100000-line file");
        for (int i = 0; i < 500; ++i)
            QTest::keyClick(&editor, Qt::Key_Down, Qt::ShiftModifier);
        check(editor.textCursor().blockNumber() == 89999 && editor.textCursor().hasSelection(),
              "Shift+Down extends a selection through 500 lines");
        check(timer.elapsed() < 5000, "1000 navigation events stay within five seconds");
        std::cout << "1000 navigation events: " << timer.elapsed() << " ms\n";
        editor.jumpToLine(90000);
        app.processEvents();
        for (int y = 2; y < editor.viewport()->height() - 2; y += 7)
            check(editor.lineAtY(y) == editor.cursorForPosition(QPoint(5, y)).blockNumber() + 1,
                  "Gutter matches every visible row after scrolling deep into a large file");
        editor.jumpToLine(90000);
        QTest::keyClick(&editor, Qt::Key_End);
        QTest::keyClick(&editor, Qt::Key_Return);
        QTest::keyClicks(&editor, "vari");
        QTest::qWait(250);
        auto* completion = editor.findChild<QCompleter*>();
        check(completion->popup()->isVisible(), "Live completion works beyond the former 2 MiB limit");
        const auto expectedPopup = editor.viewport()->mapToGlobal(editor.cursorRect().bottomLeft());
        check(std::abs(completion->popup()->pos().x() - expectedPopup.x()) <= 4,
              "Completion is aligned with the caret after the gutter offset");
        const int position = editor.textCursor().position();
        for (int i = 0; i < 20; ++i)
            QTest::keyClick(&editor, Qt::Key_Down);
        check(editor.textCursor().position() == position, "Completion arrows never move the document caret");
        QTest::keyClick(&editor, Qt::Key_Escape);
        QTest::keyClick(&editor, Qt::Key_Up);
        check(editor.textCursor().blockNumber() == 89999, "Document arrows resume after dismissal");
        editor.undo();
        editor.undo();
        editor.document()->setModified(false);

        QTextDocument second;
        second.setDocumentLayout(new QPlainTextDocumentLayout(&second));
        second.setPlainText("first\nsecond\nthird\nfourth\n");
        editor.useDocument(&second);
        auto fractionalFont = editor.font();
        fractionalFont.setPointSizeF(13.5);
        editor.setFont(fractionalFont);
        app.processEvents();
        const auto formats = second.firstBlock().layout()->formats();
        check(!formats.isEmpty() && formats.first().format.font().family() == fractionalFont.family() &&
                  formats.first().format.font().pointSizeF() == fractionalFont.pointSizeF(),
              "Syntax formats follow the editor font and fractional font size");
        editor.toggleBookmark(2);
        editor.jumpToLine(2);
        app.processEvents();
        auto* gutter = editor.findChild<QWidget*>("editorGutter");
        const int y = editor.cursorRect().center().y();
        check(editor.lineAtY(y) == 2, "Gutter hit testing matches the document after navigation");
        const int fourthY = editor.cursorRect(QTextCursor(second.findBlockByNumber(3))).center().y();
        const int numbersX = gutter->width() - 4;
        QTest::mousePress(gutter, Qt::LeftButton, Qt::NoModifier, QPoint(numbersX, fourthY));
        QTest::mouseMove(gutter, QPoint(numbersX, y));
        QTest::mouseRelease(gutter, Qt::LeftButton, Qt::NoModifier, QPoint(numbersX, y));
        check(editor.textCursor().selectedText().replace(QChar::ParagraphSeparator, '\n') ==
                      "second\nthird\nfourth\n" &&
                  editor.textCursor().position() < editor.textCursor().anchor(),
              "Dragging line numbers selects lines in reverse");
        QTest::mouseClick(gutter, Qt::LeftButton, Qt::NoModifier, QPoint(27, y));
        check(second.property("breakpoints").toList() == QVariantList{2},
              "Breakpoint has its own gutter lane");
        auto edit = QTextCursor(&second);
        edit.insertText("inserted\n");
        check(editor.bookmarkedLines() == QList<int>{3}, "Bookmark follows inserted lines");
        check(second.property("breakpoints").toList() == QVariantList{3},
              "Breakpoint follows inserted lines");
        editor.undo();
        check(editor.bookmarkedLines() == QList<int>{2}, "Undo restores bookmark position");
        check(second.property("breakpoints").toList() == QVariantList{2},
              "Undo restores breakpoint position");
        editor.selectLines(4, 2);
        check(editor.textCursor().selectedText().replace(QChar::ParagraphSeparator, '\n') ==
                      "second\nthird\nfourth\n" &&
                  editor.textCursor().position() < editor.textCursor().anchor(),
              "Reverse gutter selection preserves direction");
        const auto selected = editor.textCursor().selectedText();
        QTest::keyClick(&editor, Qt::Key_Tab);
        QTest::keyClick(&editor, Qt::Key_Backtab, Qt::ShiftModifier);
        check(editor.textCursor().selectedText() == selected,
              "Indent and outdent preserve reverse selection");
        editor.useDocument(documents.activeDocument()->text.get());
        editor.useDocument(&second);
        check(editor.bookmarkedLines() == QList<int>{2}, "Switching documents preserves bookmarks");
        second.setPlainText(QString(220, 'x') + "\nlast\n");
        QSettings().setValue("editor/wordWrap", true);
        editor.applyPreferences();
        editor.resize(400, 500);
        editor.jumpToLine(1);
        app.processEvents();
        const int secondY = editor.cursorRect(QTextCursor(second.findBlockByNumber(1))).top();
        check(secondY > editor.fontMetrics().height(),
              "The first logical line wraps to multiple visual rows");
        check(editor.lineAtY(secondY - 2) == 1 && editor.lineAtY(secondY + 2) == 2,
              "Wrapped rows share the correct logical gutter line");
        editor.selectLines(1, 1);
        check(editor.textCursor().selectedText().size() == 221,
              "Selecting a wrapped line selects all visual rows");
        QSettings().setValue("editor/wordWrap", false);
        editor.applyPreferences();
        editor.resize(1000, 700);

        editor.setPlainText("α🔥beta\nplain\n");
        editor.jumpToLine(1);
        QTest::keyClick(&editor, Qt::Key_Right);
        QTest::keyClick(&editor, Qt::Key_Right);
        check(editor.textCursor().position() == 3,
              "Horizontal movement does not split a UTF-16 surrogate pair");
        QInputMethodEvent input;
        input.setCommitString(QString::fromUtf8("文"));
        QApplication::sendEvent(&editor, &input);
        check(editor.toPlainText().startsWith(QString::fromUtf8("α🔥文beta")),
              "IME commits at the current Unicode caret");
        editor.setReadOnly(true);
        const auto unchanged = editor.toPlainText();
        QTest::keyClicks(&editor, "{\"");
        check(editor.toPlainText() == unchanged, "Pairing respects read-only buffers");
        editor.setReadOnly(false);
        editor.selectLines(1, 2);
        QApplication::clipboard()->setText("replacement\ntext");
        editor.paste();
        check(editor.toPlainText() == "replacement\ntext", "Paste replaces full-line selection atomically");
        editor.undo();
        check(editor.toPlainText() == unchanged, "Paste undo restores both lines");

        editor.setPlainText(QString(100000, 'a') + "\nlast");
        editor.jumpToLine(2);
        QTest::keyClick(&editor, Qt::Key_End);
        check(editor.textCursor().positionInBlock() == 4,
              "Navigation remains accurate after a very long logical line");
        editor.useDocument(documents.activeDocument()->text.get());
        std::cout << "PASS: 100000-line editing, completion, gutter, Unicode, markers and selection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
