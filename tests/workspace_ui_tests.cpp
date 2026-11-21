#include "CodeEditor.hpp"
#include "DatabasePanel.hpp"
#include "DatabaseSession.hpp"
#include "MainWindow.hpp"
#include "RemoteWorkspace.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}

static bool waitFor(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 12000)
        QTest::qWait(10);
    return ready();
}

static void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Write GUI fixture");
}

static QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Read GUI fixture");
    return file.readAll();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--database-worker") == 0)
        return runDatabaseWorker(argc, argv);
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("WorkspaceUi");
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QSettings().setValue("workspace/root", directory.path());
    QSettings().setValue("lsp/enabled", false);
    qputenv("TS_DISABLE_LOCAL_MODEL", "1");
    try {
        const auto database = directory.filePath("workspace.sqlite");
        auto fixture = QSqlDatabase::addDatabase("QSQLITE", "fixture");
        fixture.setDatabaseName(database);
        require(fixture.open(), "Open GUI database fixture");
        QSqlQuery sql(fixture);
        require(sql.exec("CREATE TABLE projects(id INTEGER PRIMARY KEY,name TEXT,language TEXT)"),
                "Create GUI fixture table");
        require(sql.exec("INSERT INTO projects VALUES(1,'Veyra','C++'),(2,'Notebook','SQL')"),
                "Seed GUI fixture");
        MainWindow window;
        window.show();
        window.activateWindow();
        app.processEvents();
        auto* dock = window.findChild<QDockWidget*>("databaseDock");
        require(dock, "Database dock is integrated");
        dock->show();
        dock->raise();
        auto* panel = window.findChild<DatabasePanel*>("databasePanel");
        require(panel, "Database connection panel exists");
        QTimer::singleShot(0, panel, [&] {
            auto* dialog = panel->findChild<QDialog*>("databaseConnectionDialog");
            if (!dialog)
                return;
            dialog->findChild<QComboBox*>("databaseDriver")->setCurrentText("QSQLITE");
            dialog->findChild<QLineEdit*>("databaseName")->setText(database);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        panel->findChild<QPushButton*>("databaseConnect")->click();
        auto* tables = panel->findChild<QListWidget*>("databaseTables");
        require(waitFor([&] { return tables->count() > 0; }), "Connect using native database dialog");
        tables->itemActivated(tables->item(0));
        auto* grid = panel->findChild<QTableView*>("databaseGrid");
        require(waitFor([&] { return grid->isEnabled() && grid->model()->rowCount() == 2; }),
                "Open interactive table grid");
        grid->sortByColumn(0, Qt::AscendingOrder);
        const auto index = grid->model()->index(0, 1);
        require(index.flags() & Qt::ItemIsEditable, "Transactional table exposes editable fields");
        grid->setCurrentIndex(index);
        window.activateWindow();
        window.raise();
        grid->setFocus();
        QTest::qWait(30);
        grid->edit(index);
        auto* delegate = grid->findChild<QLineEdit*>();
        require(delegate, "Table cells use an interactive editor");
        delegate->selectAll();
        QTest::keyClicks(delegate, "Edited in grid");
        QTest::keyClick(delegate, Qt::Key_Return);
        auto* apply = panel->findChild<QPushButton*>("databaseApply");
        require(waitFor([&] { return apply->isEnabled(); }), "Grid edits are staged");
        apply->click();
        require(waitFor([&] { return !grid->model()->index(0, 1).data(Qt::BackgroundRole).isValid(); }),
                "Commit grid edits through worker");
        require(sql.exec("SELECT name FROM projects WHERE id=1") && sql.next() &&
                    sql.value(0) == "Edited in grid",
                "Native Apply persists the edited cell");
        sql.finish();
        const auto screenshots = qEnvironmentVariable("VEYRA_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) {
            QDir().mkpath(screenshots);
            window.grab().save(screenshots + "/database.png");
        }
        auto* tabs = window.findChild<QTabWidget*>("databaseConnections");
        require(tabs && tabs->count() == 1, "Independent database connection tabs exist");
        qobject_cast<QPushButton*>(tabs->cornerWidget())->click();
        require(tabs->count() == 2, "Open a second independent database session");
        tabs->tabCloseRequested(1);
        window.close();
        app.processEvents();
#ifndef Q_OS_WIN
        require(argc == 2, "SSH transport fixture is required");
        write(directory.filePath("remote.cpp"), "int value = 1;\n");
        auto* remote = new RemoteWorkspace;
        remote->show();
        remote->activateWindow();
        app.processEvents();
        QTimer::singleShot(0, remote, [&] {
            auto* dialog = remote->findChild<QDialog*>("remoteConnectionDialog");
            if (!dialog)
                return;
            dialog->findChild<QLineEdit*>("remoteHost")->setText("fixture");
            dialog->findChild<QLineEdit*>("remoteRoot")->setText(directory.path());
            dialog->findChild<QLineEdit*>("remoteSshExecutable")->setText(QString::fromLocal8Bit(argv[1]));
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        remote->findChild<QPushButton*>("remoteConnect")->click();
        auto* tree = remote->findChild<QTreeWidget*>("remoteFiles");
        require(waitFor([&] { return tree->topLevelItemCount() > 0; }),
                "Connect remote workspace through native dialog");
        QTreeWidgetItem* file = nullptr;
        for (int i = 0; i < tree->topLevelItem(0)->childCount(); ++i) {
            auto* item = tree->topLevelItem(0)->child(i);
            if (item->text(0) == "remote.cpp")
                file = item;
        }
        require(file, "Remote tree contains source file");
        tree->itemActivated(file, 0);
        require(waitFor([&] { return remote->findChild<CodeEditor*>("remoteCodeEditor"); }),
                "Open remote source in shared code editor");
        auto* editor = remote->findChild<CodeEditor*>("remoteCodeEditor");
        editor->setPlainText("int value = 2;\n");
        qputenv("VEYRA_SSH_FIXTURE_DELAY", "150");
        remote->findChild<QPushButton*>("remoteSave")->click();
        auto cursor = editor->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText("// edited during upload\n");
        require(waitFor([&] { return read(directory.filePath("remote.cpp")) == "int value = 2;\n"; }),
                "Upload the captured document snapshot");
        require(waitFor([&] { return remote->findChild<QPushButton*>("remoteSave")->isEnabled(); }),
                "Finish the earlier upload acknowledgement");
        require(editor->document()->isModified(), "An older upload acknowledgement never clears newer edits");
        remote->findChild<QPushButton*>("remoteSave")->click();
        require(waitFor([&] { return !editor->document()->isModified(); }),
                "Save the newer revision explicitly");
        qunsetenv("VEYRA_SSH_FIXTURE_DELAY");
        require(read(directory.filePath("remote.cpp")).contains("edited during upload"),
                "Newer source revision reaches server");
        editor->setPlainText("int local = 3;\n");
        editor->document()->setModified(true);
        write(directory.filePath("remote.cpp"), "int server = 4;\n");
        bool compared = false;
        QTimer watcher;
        int attempts = 0;
        QObject::connect(&watcher, &QTimer::timeout, remote, [&] {
            if (auto* dialog = remote->findChild<QDialog*>("remoteComparison")) {
                compared =
                    dialog->findChild<CodeEditor*>("comparisonLocal")->toPlainText().contains("local = 3") &&
                    dialog->findChild<CodeEditor*>("comparisonServer")->toPlainText().contains("server = 4");
                watcher.stop();
                dialog->findChild<QPushButton*>("comparisonUpload")->click();
            } else if (++attempts > 300) {
                watcher.stop();
                if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    modal->reject();
            }
        });
        watcher.start(10);
        remote->findChild<QPushButton*>("remoteSave")->click();
        require(waitFor([&] { return compared && !editor->document()->isModified(); }),
                "Review both versions before overwriting a concurrent server edit");
        require(read(directory.filePath("remote.cpp")) == "int local = 3;\n",
                "Explicit conflict resolution uploads the reviewed local version");
        if (!screenshots.isEmpty())
            remote->grab().save(screenshots + "/remote.png");
        remote->close();
        app.processEvents();
#endif
        std::cout << "PASS: native SQL connection and table editing, multiple connections, remote editor, "
                     "upload revisions and conflict comparison\n";
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
