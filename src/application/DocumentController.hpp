#pragma once
#include "ts/Structures.hpp"
#include <QObject>
#include <QString>
#include <QTextDocument>
#include <functional>
#include <memory>

struct EditorDocument {
    std::unique_ptr<QTextDocument> text;
    QString path;
    QString name;
    int position = 0, anchor = 0, verticalScroll = 0, horizontalScroll = 0;
    bool trusted = false;
};
enum class CloseDecision { Save, Discard, Cancel };

class DocumentController final : public QObject {
    Q_OBJECT
    ts::Array<std::unique_ptr<EditorDocument>> documents_;
    int active_ = -1, nextUntitled_ = 1;
    std::unique_ptr<EditorDocument> makeDocument(const QString& path, const QString& text,
                                                 const QString& name);
    void wire(EditorDocument* document);
    int indexOf(const EditorDocument* document) const;

  public:
    using Decide = std::function<CloseDecision(const EditorDocument&)>;
    using SavePath = std::function<QString(const EditorDocument&)>;

    explicit DocumentController(QObject* parent = nullptr) : QObject(parent) {
    }

    int count() const {
        return static_cast<int>(documents_.size());
    }

    int activeIndex() const {
        return active_;
    }

    EditorDocument* document(int index) const {
        return index >= 0 && index < count() ? documents_[index].get() : nullptr;
    }

    EditorDocument* activeDocument() const {
        return document(active_);
    }

    int create(const QString& text = QString(), const QString& name = QString());
    bool open(const QString& path, QString& error);
    void activate(int index);
    void moveDocument(int from, int to);
    bool save(int index, const QString& requestedPath, QString& error);
    bool saveAll(const SavePath& path, QString& error);
    bool close(int index, const Decide& decide, const SavePath& path, QString& error);
    bool prepareClose(const Decide& decide, const SavePath& path, QString& error);
    bool renamePath(const QString& from, const QString& to, QString& error);
    bool saveSession(const QString& path, QString& error) const;
    bool restoreSession(const QString& path, QString& error);
    static bool readUtf8(const QString& path, QString& text, QString& error);
  signals:
    void aboutToChangeActive();
    void activeChanged(int index);
    void documentsChanged();
    void documentChanged(int index);
    void documentSaved(const QString& path);
    void documentClosed(const QString& path);
};
