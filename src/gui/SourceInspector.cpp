#include "SourceInspector.hpp"
#include "SingleFileProject.hpp"
#include "ToolDiscovery.hpp"
#include "ts/ProjectTools.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#if TS_HAS_SQLITE
#include <sqlite3.h>
#endif
namespace {
void issue(InspectionResult& result, int line, const QString& message,
           ts::RiskLevel severity = ts::RiskLevel::Critical) {
    ts::Diagnostic diagnostic{severity,
                              std::max(1, line),
                              "TOOL_DIAGNOSTIC",
                              result.engine.toStdString(),
                              message.toStdString(),
                              "Inspect the reported location and rerun the check."};
    diagnostic.origin = ts::EvidenceOrigin::Tool;
    diagnostic.confidence = ts::Confidence::High;
    result.diagnostics.pushBack(std::move(diagnostic));
}

QStringList compileFlags(const QString& file) {
    QString database = QSettings().value("lsp/cpp/compileCommands").toString();
    const auto root = QString::fromStdString(ts::findCMakeRoot(file.toStdString()));
    if (database.isEmpty() && !root.isEmpty()) {
        const auto key =
            "cmake/project/" +
            QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex());
        database = QSettings().value(key + "/build").toString();
    }
    QFile source(QDir(database).filePath("compile_commands.json"));
    if (database.isEmpty() || !source.open(QIODevice::ReadOnly) || source.size() > 16 * 1024 * 1024)
        return {};
    const auto entries = QJsonDocument::fromJson(source.readAll()).array();
    for (const auto& entry : entries) {
        const auto object = entry.toObject();
        const auto directory = object.value("directory").toString();
        if (QFileInfo(QDir(directory).filePath(object.value("file").toString())).canonicalFilePath() !=
            QFileInfo(file).canonicalFilePath())
            continue;
        QStringList raw;
        for (const auto& value : object.value("arguments").toArray())
            raw << value.toString();
        if (raw.isEmpty())
            raw = QProcess::splitCommand(object.value("command").toString());
        QStringList safe;
        for (int i = 1; i < raw.size(); ++i) {
            const auto flag = raw[i];
            if (flag.startsWith("-D") || flag.startsWith("-U") || flag.startsWith("-std=")) {
                safe << flag;
                continue;
            }
            if (flag == "-I" || flag == "-isystem" || flag == "-iquote" || flag == "-F" ||
                flag == "-isysroot" || flag == "-iframework") {
                if (i + 1 < raw.size() && !raw[i + 1].startsWith('-'))
                    safe << flag << QDir(directory).absoluteFilePath(raw[++i]);
            } else if (flag.startsWith("-I") || flag.startsWith("-F"))
                safe << flag.left(2) + QDir(directory).absoluteFilePath(flag.mid(2));
        }
        return safe;
    }
    return {};
}
}

ts::Array<ts::Diagnostic> SourceInspector::parseDiagnostics(const QString& output, int code) {
    InspectionResult result;
    result.engine = "Compiler / analyzer";
    int pythonLine = 1;
    QString rustMessage;
    const QRegularExpression location(
        R"(^.*?:(\d+)(?::\d+)?:\s*(?:(fatal error|error|warning|note)(?:\s+[^:]*)?:\s*)?(.+)$)");
    const QRegularExpression typescript(R"(^.*?\((\d+),\d+\):\s*(error|warning)\s+[^:]*:\s*(.+)$)");
    const QRegularExpression python(R"(File ".*", line (\d+))");
    const QRegularExpression php(R"((?:Parse|Fatal|Warning).*?error: (.+) in .* on line (\d+))",
                                 QRegularExpression::CaseInsensitiveOption);
    for (const auto& raw : output.split('\n')) {
        const auto line = raw.trimmed();
        auto py = python.match(line);
        if (py.hasMatch())
            pythonLine = py.captured(1).toInt();
        if (line.startsWith("error[") || line.startsWith("error:"))
            rustMessage = line;
        auto match = location.match(line);
        if (match.hasMatch() && !line.startsWith('$')) {
            const auto kind = match.captured(2);
            if (kind == "note")
                continue;
            const auto severity = kind == "warning"             ? ts::RiskLevel::Warning
                                  : code == 0 && kind.isEmpty() ? ts::RiskLevel::Info
                                                                : ts::RiskLevel::Critical;
            auto message = match.captured(3);
            if (line.startsWith("-->"))
                message = rustMessage;
            issue(result, match.captured(1).toInt(), message, severity);
        } else if ((match = typescript.match(line)).hasMatch())
            issue(result, match.captured(1).toInt(), match.captured(3),
                  match.captured(2) == "warning" ? ts::RiskLevel::Warning : ts::RiskLevel::Critical);
        else if (line.startsWith("SyntaxError:") || line.startsWith("IndentationError:") ||
                 line.startsWith("TabError:"))
            issue(result, pythonLine, line);
        else if ((match = php.match(line)).hasMatch())
            issue(result, match.captured(2).toInt(), match.captured(1));
        if (result.diagnostics.size() >= 200)
            break;
    }
    return result.diagnostics;
}

void SourceInspector::inspect(const QString& text, const QString& path,
                              std::function<void(const InspectionResult&)> done) {
    pending_ = std::make_unique<Request>(Request{text, path, std::move(done), ++generation_});
    if (runner_.busy()) {
        runner_.stop();
        return;
    }
    launch();
}

void SourceInspector::launch() {
    if (!pending_)
        return;
    const auto request = std::make_shared<Request>(std::move(*pending_));
    pending_.reset();
    auto result = std::make_shared<InspectionResult>();
    const auto language = ts::languageForPath(request->path.toStdString());
    const auto finish = [this, request, result] {
        if (request->generation == generation_ && request->done)
            request->done(*result);
        if (pending_)
            QTimer::singleShot(0, this, [this] { launch(); });
    };
    if (request->text.toUtf8().size() > 2 * 1024 * 1024) {
        result->status = "Inspection limit: 2 MiB";
        finish();
        return;
    }
    if (language.id == ts::LanguageId::Json) {
        result->engine = "Qt JSON parser";
        result->available = result->evaluated = true;
        QJsonParseError error;
        QJsonDocument::fromJson(request->text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError && !request->text.trimmed().isEmpty()) {
            QJsonParseError scalarError;
            const auto scalar = QJsonDocument::fromJson("[" + request->text.toUtf8() + "]", &scalarError);
            if (scalarError.error == QJsonParseError::NoError && scalar.array().size() == 1)
                error = QJsonParseError{};
        }
        if (error.error != QJsonParseError::NoError)
            issue(*result, 1 + request->text.toUtf8().left(error.offset).count('\n'), error.errorString());
        result->scope = "JSON syntax";
        result->status = "Completed";
        finish();
        return;
    }
#if TS_HAS_SQLITE
    if (language.id == ts::LanguageId::Sql) {
        result->engine = "SQLite parser";
        result->available = result->evaluated = true;
        result->scope = "Prepare statements without executing; table resolution requires a database schema";
        sqlite3* database = nullptr;
        if (sqlite3_open(":memory:", &database) == SQLITE_OK) {
            const auto bytes = request->text.toUtf8();
            const char* cursor = bytes.constData();
            const char* end = cursor + bytes.size();
            while (cursor < end) {
                sqlite3_stmt* statement = nullptr;
                const char* tail = nullptr;
                const int code = sqlite3_prepare_v2(database, cursor, int(end - cursor), &statement, &tail);
                if (statement)
                    sqlite3_finalize(statement);
                if (code != SQLITE_OK) {
                    const auto message = QString::fromUtf8(sqlite3_errmsg(database));
                    issue(*result, 1 + bytes.left(cursor - bytes.constData()).count('\n'), message,
                          message.contains("no such") ? ts::RiskLevel::Warning : ts::RiskLevel::Critical);
                    break;
                }
                if (!tail || tail <= cursor)
                    break;
                cursor = tail;
            }
            sqlite3_close(database);
        }
        result->status = "Completed";
        finish();
        return;
    }
#endif
    auto temporary = std::make_shared<QTemporaryDir>();
    const auto snapshot =
        QDir(temporary->path())
            .filePath(QFileInfo(request->path).fileName().isEmpty() ? "untitled.txt"
                                                                    : QFileInfo(request->path).fileName());
    QFile file(snapshot);
    if (!file.open(QIODevice::WriteOnly) || file.write(request->text.toUtf8()) < 0) {
        result->status = "Cannot create analysis snapshot";
        finish();
        return;
    }
    file.close();
    const auto build = temporary->path() + "/output";
    QDir().mkpath(build);
    auto plan = ts::buildPlanFor(snapshot.toStdString(), build.toStdString());
    ts::ProcessSpec command;
    QStringList arguments;
    if (language.id == ts::LanguageId::Cpp || language.id == ts::LanguageId::C) {
        command.valid = true;
        const bool cpp = language.id == ts::LanguageId::Cpp;
        command.program = plan.build.program;
        result->engine = cpp ? "C++ compiler" : "C compiler";
        result->scope = "Syntax and type checking with the selected toolchain";
        const auto compiler =
            ToolDiscovery::compiler(cpp ? "cpp" : "c", QString::fromStdString(command.program));
        if (QFileInfo(compiler).baseName().compare("cl", Qt::CaseInsensitive) == 0) {
            arguments = {"/nologo", "/Zs", "/W4", "/EHsc", "/I" + QFileInfo(request->path).absolutePath()};
            if (cpp)
                arguments << "/std:c++20";
        } else {
            arguments = {
                "-fsyntax-only", cpp ? "-std=c++20" : "-std=c17",        "-Wall", "-Wextra", "-Wpedantic",
                "-iquote",       QFileInfo(request->path).absolutePath()};
            arguments << compileFlags(request->path);
        }
        arguments << snapshot;
    } else if (language.id == ts::LanguageId::TypeScript) {
        command.valid = true;
        command.program = "tsc";
        arguments = {"--noEmit", "--strict", "--target", "ES2020", "--module", "commonjs", snapshot};
        result->engine = "TypeScript type checker";
        result->scope = "Single-file strict type and syntax checking";
    } else if (language.id == ts::LanguageId::Go) {
        command.valid = true;
        command.program = "go";
        arguments = {"vet", snapshot};
        result->engine = "go vet";
        result->scope = "Go type checking and vet analyzers";
    } else if (language.id == ts::LanguageId::Swift) {
        command.valid = true;
        command.program = "swiftc";
        arguments = {"-typecheck", snapshot};
        result->engine = "Swift type checker";
        result->scope = "Compiler syntax and type checking";
    } else if (language.id == ts::LanguageId::Rust) {
        command.valid = true;
        command.program = "rustc";
        arguments = {"--edition", "2021", "--emit=metadata", "-o", temporary->path() + "/snapshot.rmeta",
                     snapshot};
        result->engine = "Rust compiler";
        result->scope = "Types, ownership and borrow checking";
    } else if (language.id == ts::LanguageId::Bash && !ToolDiscovery::find("shellcheck").isEmpty()) {
        command.valid = true;
        command.program = "shellcheck";
        arguments = {"-f", "gcc", snapshot};
        result->engine = "ShellCheck";
        result->scope = "Shell static analysis";
    } else if (plan.supported && plan.build.valid) {
        command = plan.build;
        result->engine = QString::fromStdString(plan.language.name) + " compiler / parser";
        result->scope = "Single-file compiler or syntax validation";
        if (language.id == ts::LanguageId::Java) {
            command.arguments.pushBack("-Xlint:all");
            command.arguments.pushBack("-proc:none");
        }
        QString error;
        if (!prepareSingleFileProject(plan, snapshot, build, error)) {
            result->status = error;
            finish();
            return;
        }
    } else {
        result->engine = QString::fromStdString(language.name);
        result->scope = "Editor / preview and installed language-server diagnostics";
        result->status = "No standalone validator is configured for this format";
        finish();
        return;
    }
    const auto selectedCompiler = ToolDiscovery::compiler(QString::fromStdString(language.lspId),
                                                          QString::fromStdString(command.program));
    if (selectedCompiler.isEmpty()) {
        result->status = "Compiler is unavailable in the selected SDK";
        finish();
        return;
    }
    command.program = selectedCompiler.toStdString();
    if (!arguments.isEmpty()) {
        command.arguments.clear();
        for (const auto& value : arguments)
            command.arguments.pushBack(value.toStdString());
    }
    command.workingDirectory = temporary->path().toStdString();
    const auto executable = ToolDiscovery::find(QString::fromStdString(command.program));
    if (executable.isEmpty()) {
        result->status = "Required analysis tool is missing: " + QString::fromStdString(command.program);
        finish();
        return;
    }
    command.program = executable.toStdString();
    result->available = true;
    ts::Array<ts::ProcessSpec> commands;
    commands.pushBack(std::move(command));
    QString error;
    if (!runner_.startCommands(
            std::move(commands), false,
            [result](const QString& chunk) {
                if (result->output.size() < 512 * 1024)
                    result->output += chunk;
            },
            [request, result, temporary, snapshot, finish](bool success, int code) {
                result->exitCode = code;
                result->evaluated = code != 124 && code != 130 && code != -1;
                result->status = success       ? "Completed"
                                 : code == 124 ? "Time limit reached; analysis incomplete"
                                 : code == 130 ? "Cancelled"
                                 : code == -1  ? "Could not launch analysis tool"
                                               : "Compiler/analyzer reported diagnostics (exit " +
                                                    QString::number(code) + ")";
                result->diagnostics = parseDiagnostics(result->output, code);
                for (auto& diagnostic : result->diagnostics)
                    diagnostic.title = result->engine.toStdString();
                if (!success && result->evaluated && result->diagnostics.empty())
                    issue(*result, 1, result->output.right(6000));
                result->output.replace(snapshot, request->path);
                finish();
            },
            error)) {
        result->available = false;
        result->status = error;
        finish();
    }
}
