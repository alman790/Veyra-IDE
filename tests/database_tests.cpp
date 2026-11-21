#include "DatabasePanel.hpp"
#include "DatabaseSession.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
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
    while (!ready() && timer.elapsed() < 10000)
        QTest::qWait(10);
    return ready();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--database-worker") == 0)
        return runDatabaseWorker(argc, argv);
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("Database");
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    UiLanguage::initialize();
    try {
        require(QSqlDatabase::isDriverAvailable("QSQLITE"), "SQLite driver is required");
        const auto path = directory.filePath("test database.sqlite");
        auto checkDb = QSqlDatabase::addDatabase("QSQLITE", "assertions");
        checkDb.setDatabaseName(path);
        require(checkDb.open(), "Open fixture database");
        QSqlQuery setup(checkDb);
        require(setup.exec("CREATE TABLE \"odd table\" (id INTEGER PRIMARY KEY, name TEXT NOT NULL, quantity "
                           "INTEGER, note TEXT, payload BLOB)"),
                "Create quoted table");
        for (int i = 0; i < 8; ++i) {
            setup.prepare("INSERT INTO \"odd table\" VALUES (?, ?, ?, ?, ?)");
            setup.addBindValue(i + 1);
            setup.addBindValue(QString("row %1").arg(i + 1));
            setup.addBindValue(i);
            setup.addBindValue("seed");
            setup.addBindValue(QByteArray::fromHex("000102"));
            require(setup.exec(), "Insert fixture row");
        }
        DatabaseSession session;
        QJsonObject result;
        QString failure;
        int responses = 0, failures = 0;
        QObject::connect(&session, &DatabaseSession::resultReady, &app, [&](const QJsonObject& value) {
            result = value;
            ++responses;
        });
        QObject::connect(&session, &DatabaseSession::failed, &app, [&](const QString& value) {
            failure = value;
            ++failures;
        });
        session.connectTo({"QSQLITE", path, {}, {}, {}, {}, {}, -1, 10});
        require(waitFor([&] { return responses + failures > 0; }) && failure.isEmpty() && session.connected(),
                "Connect through isolated SQL worker");
        require(result.value("tables").toArray().contains("odd table"),
                "Enumerate schema through driver metadata");
        int previous = responses;
        session.browse("odd table", 0, 3);
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty(),
                "Browse first page");
        require(result.value("rows").toArray().size() == 3 && result.value("more").toBool(),
                "Bounded table pagination");
        DatabaseGridModel model;
        model.load(result);
        require(!(model.flags(model.index(0, 0)) & Qt::ItemIsEditable) &&
                    (model.flags(model.index(0, 1)) & Qt::ItemIsEditable) &&
                    !(model.flags(model.index(0, 4)) & Qt::ItemIsEditable),
                "Keys and binary cells are protected");
        require(model.setData(model.index(0, 1), "renamed") && model.dirty(),
                "Stage edits before applying them");
        previous = responses;
        session.apply(result.value("page").toInt(), model.changes());
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty() &&
                    result.value("saved").toBool(),
                "Apply bound cell updates transactionally");
        QSqlQuery read(checkDb);
        require(read.exec("SELECT name FROM \"odd table\" WHERE id=1") && read.next() &&
                    read.value(0) == "renamed",
                "Persist edited value");
        read.finish();
        model.load(result);
        model.setNull(model.index(0, 3));
        previous = responses;
        session.apply(result.value("page").toInt(), model.changes());
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty() &&
                    result.value("rows").toArray()[0].toArray()[3].toObject().value("null").toBool(),
                "Preserve SQL NULL separately from empty strings");
        model.load(result);
        model.setData(model.index(0, 1), "first pending");
        model.setData(model.index(1, 1), "second pending");
        require(setup.exec("UPDATE \"odd table\" SET name='concurrent' WHERE id=2"),
                "Create concurrent server edit");
        const int failuresBefore = failures;
        session.apply(result.value("page").toInt(), model.changes());
        require(waitFor([&] { return failures > failuresBefore; }) &&
                    failure.contains("changed on the server"),
                "Reject stale server values");
        require(read.exec("SELECT name FROM \"odd table\" WHERE id=1") && read.next() &&
                    read.value(0) == "renamed",
                "Roll back every staged update on conflict");
        read.finish();
        failure.clear();
        previous = responses;
        session.browse("odd table", 3, 3);
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty() &&
                    result.value("rows").toArray()[0].toArray()[0].toObject().value("text") == "4",
                "Page offsets retain stable primary-key ordering");
        previous = responses;
        session.query("SELECT count(*) AS total FROM \"odd table\"");
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty() &&
                    result.value("rows").toArray()[0].toArray()[0].toObject().value("text") == "8" &&
                    !result.value("editable").toBool(),
                "Run SQL with read-only result grids");
        require(setup.exec("CREATE TABLE replay_guard (value INTEGER)"), "Create SQL replay fixture");
        previous = responses;
        session.query("INSERT INTO replay_guard VALUES (1),(2),(3) RETURNING value", 0, 1);
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty() &&
                    result.value("more").toBool(),
                "Cache data-changing query results");
        previous = responses;
        session.queryPage(1, 1);
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty(),
                "Page SQL results without replaying the statement");
        previous = responses;
        session.queryPage(0, 1);
        require(waitFor([&] { return responses > previous || !failure.isEmpty(); }) && failure.isEmpty(),
                "Return to first cached page without replaying a write");
        require(read.exec("SELECT count(*) FROM replay_guard") && read.next() && read.value(0).toInt() == 3,
                "Result navigation never repeats INSERT");
        read.finish();
        session.query("WITH RECURSIVE t(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM t) SELECT sum(x) FROM t");
        QTest::qWait(100);
        QElapsedTimer cancellation;
        cancellation.start();
        session.cancel();
        require(!session.busy() && !session.connected() && cancellation.elapsed() < 200,
                "Cancel a blocked driver without blocking the IDE");
        std::cout << "PASS: SQL worker, metadata, pagination, staged edits, NULL, conflict rollback and "
                     "cancellation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
