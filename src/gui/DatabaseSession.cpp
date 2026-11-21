#include "DatabaseSession.hpp"
#include "ToolDiscovery.hpp"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlField>
#include <QSqlIndex>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <iostream>

namespace {
QJsonObject error(const QString& message) {
    return {{"ok", false}, {"error", message}};
}

class DatabaseWorker {
    QSqlDatabase database_;
    QSqlRecord fields_;
    QSqlIndex primary_;
    QList<QVariantList> rows_;
    QString table_, sql_;
    int page_ = 0, offset_ = 0, limit_ = 200;
    bool editable_ = false;
    QJsonObject queryCache_;

    QJsonObject queryPage(int offset, int limit) {
        if (queryCache_.value("kind") != "page")
            return error("Run a SQL statement before browsing its results.");
        auto result = queryCache_;
        const auto source = result.value("rows").toArray();
        offset = qBound(0, offset, int(source.size()));
        limit = qBound(1, limit, 2000);
        QJsonArray rows;
        for (int i = offset; i < qMin(int(source.size()), offset + limit); ++i)
            rows.append(source[i]);
        result["rows"] = rows;
        result["offset"] = offset;
        result["limit"] = limit;
        result["more"] = offset + rows.size() < source.size();
        result["page"] = ++page_;
        result["resultLimited"] = queryCache_.value("more");
        return result;
    }

    QString field(const QString& name) const {
        return database_.driver()->escapeIdentifier(name, QSqlDriver::FieldName);
    }

    QJsonObject fetch(const QString& sql, int offset, int limit, bool table) {
        editable_ = false;
        rows_.clear();
        ++page_;
        offset_ = qBound(0, offset, 10000000);
        limit_ = qBound(1, limit, 10000);
        QSqlQuery query(database_);
        query.setForwardOnly(true);
        query.setNumericalPrecisionPolicy(QSql::HighPrecision);
        primary_ = table ? database_.primaryIndex(table_) : QSqlIndex();
        if (!query.exec(sql))
            return error(query.lastError().text());
        if (!query.isSelect()) {
            return {{"ok", true}, {"kind", "affected"}, {"affected", double(query.numRowsAffected())}};
        }
        fields_ = query.record();
        editable_ = table && !primary_.isEmpty() && database_.driver()->hasFeature(QSqlDriver::Transactions);
        QJsonArray columns;
        for (int column = 0; column < fields_.count(); ++column) {
            const auto entry = fields_.field(column);
            columns.append(
                QJsonObject{{"name", entry.name()}, {"type", QString::fromLatin1(entry.metaType().name())}});
        }
        int skipped = 0;
        while (skipped < offset_ && query.next())
            ++skipped;
        QJsonArray values;
        qsizetype bytes = 0;
        while (rows_.size() < limit_ && bytes < 8 * 1024 * 1024 && query.next()) {
            QVariantList original;
            QJsonArray row;
            for (int column = 0; column < fields_.count(); ++column) {
                const auto value = query.value(column);
                if (editable_)
                    original.append(value);
                const bool binary = value.metaType().id() == QMetaType::QByteArray;
                const auto text = binary ? QString::number(value.toByteArray().size()) : value.toString();
                const bool truncated = text.size() > 16384;
                const bool key = primary_.indexOf(fields_.fieldName(column)) >= 0;
                row.append(QJsonObject{{"text", text.left(16384)},
                                       {"null", value.isNull()},
                                       {"binary", binary},
                                       {"truncated", truncated},
                                       {"editable", editable_ && !binary && !truncated && !key}});
                bytes += binary ? value.toByteArray().size() : text.size() * 2;
            }
            rows_.append(original);
            values.append(row);
        }
        const bool more = query.next();
        if (query.lastError().isValid())
            return error(query.lastError().text());
        return {{"ok", true},
                {"kind", "page"},
                {"page", page_},
                {"offset", offset_},
                {"limit", limit_},
                {"more", more},
                {"editable", editable_},
                {"columns", columns},
                {"rows", values},
                {"table", table ? table_ : QString()}};
    }

    QJsonObject apply(const QJsonObject& request) {
        if (!editable_ || request.value("page").toInt(-1) != page_)
            return error("Reload this table before applying changes.");
        const auto changes = request.value("changes").toArray();
        if (changes.isEmpty() || changes.size() > 2000)
            return error("Select between 1 and 2000 cell changes.");
        if (!database_.transaction())
            return error(database_.lastError().text());
        for (const auto& entry : changes) {
            const auto change = entry.toObject();
            const int row = change.value("row").toInt(-1), column = change.value("column").toInt(-1);
            if (row < 0 || row >= rows_.size() || column < 0 || column >= fields_.count() ||
                primary_.indexOf(fields_.fieldName(column)) >= 0 ||
                rows_[row][column].metaType().id() == QMetaType::QByteArray) {
                database_.rollback();
                return error("This cell cannot be edited.");
            }
            QVariant value;
            if (!change.value("null").toBool()) {
                value = change.value("text").toString();
                const auto type = fields_.field(column).metaType();
                if (type.isValid() && type.id() != QMetaType::Double && type.id() != QMetaType::Float &&
                    !value.convert(type)) {
                    database_.rollback();
                    return error("The new value does not match the column type.");
                }
            }
            QStringList conditions;
            QVariantList bindings{value};
            for (int key = 0; key < primary_.count(); ++key) {
                const int index = fields_.indexOf(primary_.fieldName(key));
                if (index < 0) {
                    database_.rollback();
                    return error("The table key is unavailable. Reload its schema.");
                }
                const auto original = rows_[row][index];
                conditions.append(field(primary_.fieldName(key)) + (original.isNull() ? " IS NULL" : " = ?"));
                if (!original.isNull())
                    bindings.append(original);
            }
            const auto original = rows_[row][column];
            conditions.append(field(fields_.fieldName(column)) + (original.isNull() ? " IS NULL" : " = ?"));
            if (!original.isNull())
                bindings.append(original);
            QSqlQuery update(database_);
            const auto statement =
                "UPDATE " + database_.driver()->escapeIdentifier(table_, QSqlDriver::TableName) + " SET " +
                field(fields_.fieldName(column)) + " = ? WHERE " + conditions.join(" AND ");
            bool ok = update.prepare(statement);
            for (const auto& binding : bindings)
                update.addBindValue(binding);
            ok = ok && update.exec();
            if (!ok || update.numRowsAffected() != 1) {
                const auto message = ok ? QString("The row changed on the server. No edits were committed.")
                                        : update.lastError().text();
                database_.rollback();
                return error(message);
            }
        }
        if (!database_.commit()) {
            const auto message = database_.lastError().text();
            database_.rollback();
            return error(message);
        }
        auto result = fetch(sql_, offset_, limit_, true);
        result["saved"] = true;
        return result;
    }

  public:
    QJsonObject handle(const QJsonObject& request) {
        const auto operation = request.value("operation").toString();
        if (operation == "connect") {
            database_ = QSqlDatabase::addDatabase(request.value("driver").toString(), "worker");
            database_.setDatabaseName(request.value("database").toString());
            database_.setHostName(request.value("host").toString());
            database_.setPort(request.value("port").toInt(-1));
            database_.setUserName(request.value("user").toString());
            database_.setConnectOptions(request.value("options").toString());
            if (!database_.open(request.value("user").toString(), request.value("password").toString()))
                return error(database_.lastError().text());
            return {{"ok", true},
                    {"kind", "connected"},
                    {"tables", QJsonArray::fromStringList(database_.tables(
                                   static_cast<QSql::TableType>(QSql::Tables | QSql::Views)))}};
        }
        if (!database_.isOpen())
            return error("Connect to a database first.");
        if (operation == "apply")
            return apply(request);
        if (operation == "query_page")
            return queryPage(request.value("offset").toInt(), request.value("limit").toInt(200));
        const int offset = request.value("offset").toInt(), limit = request.value("limit").toInt(200);
        if (operation == "table") {
            queryCache_ = {};
            table_ = request.value("table").toString();
            if (!database_.tables(static_cast<QSql::TableType>(QSql::Tables | QSql::Views)).contains(table_))
                return error("The selected table is unavailable.");
            const auto record = database_.record(table_);
            sql_ = database_.driver()->sqlStatement(QSqlDriver::SelectStatement, table_, record, false);
            const auto keys = database_.primaryIndex(table_);
            QStringList order;
            for (int i = 0; i < keys.count(); ++i)
                order.append(field(keys.fieldName(i)));
            if (!order.isEmpty())
                sql_ += " ORDER BY " + order.join(", ");
            return fetch(sql_, offset, qBound(1, limit, 2000), true);
        }
        if (operation == "query") {
            queryCache_ = {};
            table_.clear();
            sql_ = request.value("sql").toString();
            if (sql_.trimmed().isEmpty())
                return error("Enter a SQL statement.");
            const bool transactional = database_.driver()->hasFeature(QSqlDriver::Transactions);
            if (transactional && !database_.transaction())
                return error(database_.lastError().text());
            auto result = fetch(sql_, 0, 5000, false);
            if (transactional) {
                if (!result.value("ok").toBool())
                    database_.rollback();
                else if (!database_.commit()) {
                    const auto message = database_.lastError().text();
                    database_.rollback();
                    result = error(message);
                }
            }
            if (result.value("ok").toBool() && result.value("kind") == "page") {
                queryCache_ = result;
                return queryPage(0, limit);
            }
            return result;
        }
        return error("Unknown database operation.");
    }
};
}

int runDatabaseWorker(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto plugins = qEnvironmentVariable("VEYRA_SQL_PLUGIN_PATH");
    if (!plugins.isEmpty())
        QCoreApplication::addLibraryPath(plugins);
    DatabaseWorker worker;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.size() > 4 * 1024 * 1024)
            return 2;
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        auto result = worker.handle(request);
        result["id"] = request.value("id");
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << std::endl;
    }
    return 0;
}

DatabaseSession::DatabaseSession(QObject* parent, QString executable)
    : QObject(parent),
      executable_(executable.isEmpty() ? QCoreApplication::applicationFilePath() : std::move(executable)) {
    tree_.configure(process_);
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        cancel();
        emit failed("The database request timed out. The connection was closed.");
    });
    connect(&process_, &QProcess::started, this, [this] {
        QString message;
        if (!tree_.attach(process_.processId(), message)) {
            cancel();
            emit failed(message);
        }
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &DatabaseSession::consume);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { process_.readAllStandardError(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (busy_) {
            const auto message = process_.errorString();
            cancel();
            emit failed(message);
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this] {
        const bool unfinished = busy_;
        deadline_.stop();
        tree_.reset();
        busy_ = connected_ = false;
        emit busyChanged(false);
        emit connectedChanged(false);
        if (unfinished)
            emit failed("The database worker stopped before completing the request.");
    });
}

DatabaseSession::~DatabaseSession() {
    blockSignals(true);
    process_.disconnect(this);
    cancel();
    process_.waitForFinished(1500);
}

void DatabaseSession::request(QJsonObject message) {
    if (busy_ || process_.state() == QProcess::NotRunning)
        return;
    operation_ = message.value("operation").toString();
    message["id"] = ++serial_;
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > 4 * 1024 * 1024) {
        emit failed("The database request exceeds 4 MiB. Apply fewer changes at a time.");
        return;
    }
    busy_ = true;
    emit busyChanged(true);
    process_.write(bytes);
    deadline_.start(timeout_);
}

void DatabaseSession::connectTo(const DatabaseConnection& config) {
    if (process_.state() != QProcess::NotRunning) {
        emit failed("Disconnect the current database before opening another connection.");
        return;
    }
    auto environment = ToolDiscovery::environment();
    environment.insert("VEYRA_SQL_PLUGIN_PATH", config.pluginPath);
    process_.setProcessEnvironment(environment);
    pending_.clear();
    timeout_ = qBound(1, config.timeoutSeconds, 600) * 1000;
    process_.start(executable_, {"--database-worker"});
    request({{"operation", "connect"},
             {"driver", config.driver},
             {"database", config.database},
             {"host", config.host},
             {"port", config.port},
             {"user", config.user},
             {"password", config.password},
             {"options", config.options}});
}

void DatabaseSession::browse(const QString& table, int offset, int limit) {
    request({{"operation", "table"}, {"table", table}, {"offset", offset}, {"limit", limit}});
}

void DatabaseSession::query(const QString& sql, int offset, int limit) {
    request({{"operation", "query"}, {"sql", sql}, {"offset", offset}, {"limit", limit}});
}

void DatabaseSession::queryPage(int offset, int limit) {
    request({{"operation", "query_page"}, {"offset", offset}, {"limit", limit}});
}

void DatabaseSession::apply(int page, const QJsonArray& changes) {
    request({{"operation", "apply"}, {"page", page}, {"changes", changes}});
}

void DatabaseSession::cancel() {
    deadline_.stop();
    busy_ = connected_ = false;
    tree_.terminate();
    process_.kill();
    emit busyChanged(false);
    emit connectedChanged(false);
}

void DatabaseSession::consume() {
    pending_ += process_.readAllStandardOutput();
    if (pending_.size() > 32 * 1024 * 1024) {
        cancel();
        emit failed("The database result exceeded its size limit.");
        return;
    }
    int newline;
    while ((newline = pending_.indexOf('\n')) >= 0) {
        const auto result = QJsonDocument::fromJson(pending_.left(newline)).object();
        pending_.remove(0, newline + 1);
        if (result.value("id").toInt(-1) != serial_)
            continue;
        deadline_.stop();
        busy_ = false;
        emit busyChanged(false);
        if (!result.value("ok").toBool()) {
            if (!connected_)
                cancel();
            emit failed(result.value("error").toString());
            continue;
        }
        if (result.value("kind") == "connected") {
            connected_ = true;
            emit connectedChanged(true);
        }
        emit resultReady(result);
    }
}
