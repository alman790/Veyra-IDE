#pragma once
#include "CodeRunner.hpp"
#include "ts/Analyzer.hpp"
#include <QTemporaryDir>
#include <memory>

struct InspectionResult {
    bool available = false, evaluated = false;
    QString engine, scope, output, status;
    int exitCode = 0;
    ts::Array<ts::Diagnostic> diagnostics;
};

class SourceInspector final : public QObject {
    Q_OBJECT
    CodeRunner runner_;
    unsigned generation_ = 0;

    struct Request {
        QString text, path;
        std::function<void(const InspectionResult&)> done;
        unsigned generation = 0;
    };

    std::unique_ptr<Request> pending_;
    void launch();

  public:
    explicit SourceInspector(QObject* parent = nullptr) : QObject(parent) {
        runner_.setTimeouts(15000, 15000);
    }

    void inspect(const QString& text, const QString& path, std::function<void(const InspectionResult&)> done);

    void cancel() {
        ++generation_;
        pending_.reset();
        runner_.stop();
    }

    bool busy() const {
        return runner_.busy();
    }

    static ts::Array<ts::Diagnostic> parseDiagnostics(const QString& output, int exitCode);
};
