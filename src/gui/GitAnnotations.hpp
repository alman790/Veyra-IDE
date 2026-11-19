#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTextDocument>
#include <QTimer>

struct LineAttribution {
    QString commit;
    QString author;
    QString summary;
    qint64 timestamp = 0;
};

class GitAnnotations final : public QObject {
    Q_OBJECT
    QPointer<QTextDocument> document_;
    QPointer<QProcess> process_;
    QString path_, error_;
    QVector<LineAttribution> lines_;
    QTimer debounce_, refresh_;
    QMetaObject::Connection changed_;
    quint64 generation_ = 0;
    bool enabled_ = false;
    void invalidate();
    void request();

  public:
    explicit GitAnnotations(QObject* parent = nullptr);
    ~GitAnnotations() override;
    void setDocument(QTextDocument* document, const QString& path);
    void setEnabled(bool enabled);

    bool enabled() const {
        return enabled_;
    }

    const QVector<LineAttribution>& lines() const {
        return lines_;
    }

    QString error() const;

    static QVector<LineAttribution> parse(const QByteArray& output, int lineCount);

  signals:
    void updated();
};
