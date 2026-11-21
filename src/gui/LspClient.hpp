#pragma once
#include "../platform/ProcessTree.hpp"
#include "JsonRpcFramer.hpp"
#include "ts/LanguageTools.hpp"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <functional>

struct LspTransport {
    QString program, workspace, sdkRoot;
    QStringList arguments;
};

class LspClient final : public QObject {
    LspTransport transport_;
    QString fileUri(const QString& path) const;

    struct Document {
        QString text;
        QString language;
        int version = 1;
        bool opened = false;
        QJsonArray diagnostics;
        int parsedVersion = -1;
    };

    QProcess server_;
    ProcessTree tree_;
    QTimer shutdownDeadline_;
    QTimer completionRetry_;
    int completionLine_ = 0, completionColumn_ = 0, completionRetries_ = 0;
    JsonRpcFramer framer_;

    struct PendingRequest {
        QString uri;
        int version;
        std::function<void(const QJsonValue&)> callback;
    };

    QHash<int, PendingRequest> requests_;
    QHash<QString, Document> documents_;
    int nextRequest_ = 1, initializeRequest_ = -1, shutdownRequest_ = -1, completionRequest_ = -1,
        completionVersion_ = 0;
    bool initialized_ = false, stopping_ = false, destroying_ = false;
    QByteArray environmentKey_;
    QJsonObject serverSettings_, initializationOptions_;
    QHash<QString, QJsonObject> completionEdits_;
    QStringList semanticTypes_;
    QString documentUri_, languageId_, workspace_, command_, completionUri_, positionEncoding_ = "utf-16";
    std::function<void(const QString&)> status_;
    std::function<void(const QStringList&)> completions_;
    std::function<void(const QString&, int, const QString&)> diagnostic_;
    std::function<void()> pendingStart_;
    void send(const QJsonObject& request);
    void notification(const QString& method, const QJsonObject& params);
    void accept(const QJsonObject& message);
    void opened(const QString& uri);
    void cancelCompletion();
    void publishCached(const QString& uri);
    void initialize();

  public:
    explicit LspClient(QObject* parent = nullptr);

    void setTransport(const LspTransport& transport) {
        transport_ = transport;
    }

    ~LspClient() override;

    bool active() const noexcept {
        return server_.state() != QProcess::NotRunning;
    }

    bool ready() const noexcept {
        return initialized_ && !stopping_;
    }

    bool startFile(const QString& sourcePath, const QString& text, const ts::LanguageInfo& language,
                   std::function<void(const QString&)> status,
                   std::function<void(const QStringList&)> completions,
                   std::function<void(const QString&, int, const QString&)> diagnostic);
    void changeText(const QString& text);

    QHash<QString, QJsonObject> completionEdits() const {
        return completionEdits_;
    }

    QStringList semanticTypes() const {
        return semanticTypes_;
    }

    void saveFile(const QString& sourcePath, const QString& text);
    void closeFile(const QString& sourcePath);
    void requestCompletion(int zeroBasedLine, int utf16Column);
    void requestDocument(const QString& method, int line, int column, const QJsonObject& extra,
                         std::function<void(const QJsonValue&)> callback);
    int documentVersion(const QString& path) const;
    void stop();
};
