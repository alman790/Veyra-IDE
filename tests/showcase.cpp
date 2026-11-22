#include "CodeEditor.hpp"
#include "MainWindow.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QCompleter>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <iostream>
#include <stdexcept>

// Records the actual widgets and model output, using a disposable workspace.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("Showcase-" + QUuid::createUuid().toString(QUuid::Id128));
    const auto sessionDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto cleanup = qScopeGuard([&] { QDir(sessionDirectory).removeRecursively(); });
    QTemporaryDir workspace(QDir::tempPath() + "/veyra-workspace-XXXXXX");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, workspace.path());
    QSettings().setValue("workspace/root", workspace.path());
    QSettings().setValue("lsp/enabled", false);
    QSettings().setValue("assistant/review", false);
    qunsetenv("TS_DISABLE_LOCAL_MODEL");
    const auto frames = qEnvironmentVariable("VEYRA_DEMO_FRAMES");
    if (frames.isEmpty())
        return 1;
    QDir().mkpath(frames);
    try {
        QFile cpp(workspace.filePath("counter.cpp")), model(workspace.filePath("add.py"));
        if (!cpp.open(QIODevice::WriteOnly) || !model.open(QIODevice::WriteOnly))
            throw std::runtime_error("Create showcase workspace");
        cpp.write("#include <thread>\n\nint counter = 0;\n\nvoid worker() {\n    ++counter;\n}\n\nint main() "
                  "{\n    std::thread first(worker);\n    std::thread second(worker);\n    first.join();\n   "
                  " second.join();\n}\n");
        cpp.close();
        model.write("def add(a, b):\n    return \n");
        model.close();
        MainWindow window;
        window.resize(1280, 800);
        window.show();
        window.activateWindow();
        auto* editor = window.findChild<CodeEditor*>("codeEditor");
        int number = 0;
        QElapsedTimer clock;
        clock.start();
        QFile timeline(frames + "/timeline.txt");
        if (!timeline.open(QIODevice::WriteOnly))
            throw std::runtime_error("Create recording timeline");
        QTimer recorder;
        QObject::connect(&recorder, &QTimer::timeout, &window, [&] {
            auto frame = window.grab();
            auto* completion = editor->findChild<QCompleter*>();
            if (auto* popup = completion ? completion->popup() : nullptr; popup && popup->isVisible()) {
                QPainter painter(&frame);
                painter.drawPixmap(window.mapFromGlobal(popup->mapToGlobal(QPoint())), popup->grab());
            }
            const auto name = QString("frame-%1.png").arg(number++, 5, 10, QLatin1Char('0'));
            frame.save(frames + '/' + name);
            timeline.write((QString::number(clock.elapsed()) + ' ' + name + '\n').toUtf8());
        });
        recorder.start(125);
        QTest::qWait(1500);
        editor->openFileRequested(cpp.fileName());
        editor->jumpToLine(6);
        editor->setFocus();
        auto* hint = window.findChild<QLabel*>("inlineThreadHint");
        QElapsedTimer deadline;
        deadline.start();
        while (hint->isHidden() && deadline.elapsed() < 5000)
            QTest::qWait(20);
        if (hint->isHidden())
            throw std::runtime_error("Live thread feedback must be visible");
        QTest::qWait(3500);
        editor->jumpToLine(6);
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertText("\n    ");
        editor->setTextCursor(cursor);
        QTest::keyClicks(editor, "cou", Qt::NoModifier, 170);
        QTest::qWait(250);
        if (!editor->findChild<QCompleter*>()->popup()->isVisible())
            throw std::runtime_error("The live completion menu must remain visible while recording");
        QTest::qWait(1700);
        QTest::keyClick(editor, Qt::Key_Escape);
        editor->document()->setModified(false);
        editor->openFileRequested(model.fileName());
        window.activateWindow();
        window.raise();
        cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.movePosition(QTextCursor::Left);
        editor->setTextCursor(cursor);
        editor->setFocus();
        QTest::qWait(100);
        QTest::keyClicks(editor, "a");
        deadline.restart();
        while (editor->property("ghostText").toString().isEmpty() && deadline.elapsed() < 15000)
            QTest::qWait(20);
        if (editor->property("ghostText").toString().isEmpty())
            throw std::runtime_error("Bundled model must supply a real inline suggestion");
        std::cout << "Recorded local continuation: " << editor->property("ghostText").toString().toStdString()
                  << '\n';
        QTest::qWait(2300);
        QTest::keyClick(editor, Qt::Key_Tab);
        QTest::qWait(qMax(2000LL, 23000LL - clock.elapsed()));
        recorder.stop();
        timeline.close();
        editor->document()->setModified(false);
        std::cout << "Captured " << number << " actual UI frames over " << clock.elapsed() << " ms\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
