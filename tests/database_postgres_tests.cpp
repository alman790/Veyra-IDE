#include "DatabaseSession.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>
#include <QUuid>
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
    while (!ready() && timer.elapsed() < 15000)
        QTest::qWait(10);
    return ready();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--database-worker") == 0)
        return runDatabaseWorker(argc, argv);
    QCoreApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("Postgres");
    if (!qEnvironmentVariableIsSet("VEYRA_TEST_POSTGRES_PORT")) {
        std::cout << "SKIP: dedicated PostgreSQL fixture is unavailable\n";
        return 77;
    }
    const auto database = qEnvironmentVariable("VEYRA_TEST_POSTGRES_DATABASE", "veyra_fixture"),
               user = qEnvironmentVariable("VEYRA_TEST_POSTGRES_USER", "veyra_fixture"),
               password = qEnvironmentVariable("VEYRA_TEST_POSTGRES_PASSWORD", "fixture");
    const int port = qEnvironmentVariableIntValue("VEYRA_TEST_POSTGRES_PORT");
    const auto table = "veyra_fixture_" + QUuid::createUuid().toString(QUuid::Id128);
    QSqlDatabase fixture;
    try {
        require(QSqlDatabase::isDriverAvailable("QPSQL"), "PostgreSQL driver is required for this fixture");
        fixture = QSqlDatabase::addDatabase("QPSQL", "fixture");
        fixture.setHostName("127.0.0.1");
        fixture.setPort(port);
        fixture.setDatabaseName(database);
        fixture.setUserName(user);
        require(fixture.open(user, password), "Open dedicated PostgreSQL fixture");
        QSqlQuery setup(fixture);
        require(setup.exec("CREATE TABLE " + table + " (id BIGSERIAL PRIMARY KEY, note TEXT)"),
                "Create isolated PostgreSQL table");
        require(setup.exec("INSERT INTO " + table + " (id,note) VALUES (9007199254740993,NULL)"),
                "Seed exact bigint key");
        DatabaseSession session;
        QJsonObject result;
        QString error;
        int responses = 0, failures = 0;
        QObject::connect(&session, &DatabaseSession::resultReady, &app, [&](const QJsonObject& value) {
            result = value;
            ++responses;
        });
        QObject::connect(&session, &DatabaseSession::failed, &app, [&](const QString& value) {
            error = value;
            ++failures;
        });
        session.connectTo({"QPSQL", database, "127.0.0.1", user, password, {}, {}, port, 15});
        require(waitFor([&] { return responses + failures > 0; }) && error.isEmpty(),
                "Connect PostgreSQL worker");
        int previous = responses;
        session.browse(table);
        require(waitFor([&] { return responses > previous || !error.isEmpty(); }) && error.isEmpty() &&
                    result.value("editable").toBool(),
                "Discover PostgreSQL table key and transactions");
        require(result.value("rows").toArray()[0].toArray()[0].toObject().value("text") == "9007199254740993",
                "Preserve bigint values beyond JSON numeric precision");
        previous = responses;
        session.apply(
            result.value("page").toInt(),
            QJsonArray{QJsonObject{{"row", 0}, {"column", 1}, {"text", "changed"}, {"null", false}}});
        require(waitFor([&] { return responses > previous || !error.isEmpty(); }) && error.isEmpty() &&
                    result.value("saved").toBool(),
                "Bind exact keys and update nullable PostgreSQL cells");
        require(setup.exec("UPDATE " + table + " SET note='concurrent' WHERE id=9007199254740993"),
                "Create concurrent PostgreSQL write");
        const int before = failures;
        session.apply(result.value("page").toInt(),
                      QJsonArray{QJsonObject{{"row", 0}, {"column", 1}, {"text", "stale"}, {"null", false}}});
        require(waitFor([&] { return failures > before; }) && error.contains("changed on the server"),
                "Detect PostgreSQL row conflicts");
        error.clear();
        previous = responses;
        session.query("INSERT INTO " + table + " (note) VALUES ('first'),('second') RETURNING id", 0, 1);
        require(waitFor([&] { return responses > previous || !error.isEmpty(); }) && error.isEmpty() &&
                    result.value("more").toBool(),
                "Display a data-changing statement with RETURNING");
        previous = responses;
        session.queryPage(1, 1);
        require(waitFor([&] { return responses > previous || !error.isEmpty(); }) && error.isEmpty(),
                "Page cached RETURNING results");
        previous = responses;
        session.queryPage(0, 1);
        require(waitFor([&] { return responses > previous || !error.isEmpty(); }) && error.isEmpty(),
                "Return to a cached result page without executing SQL again");
        require(setup.exec("SELECT count(*) FROM " + table) && setup.next() && setup.value(0).toInt() == 3,
                "Result pagination never replays writes");
        setup.finish();
        session.cancel();
        require(setup.exec("DROP TABLE " + table), "Remove dedicated fixture table");
        std::cout << "PASS: real PostgreSQL metadata, bigint keys, NULL edits, conflicts and write-safe "
                     "result paging\n";
    } catch (const std::exception& exception) {
        if (fixture.isOpen()) {
            QSqlQuery cleanup(fixture);
            cleanup.exec("DROP TABLE IF EXISTS " + table);
        }
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
