#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

class JsonRpcFramer final {
    QByteArray buffer_;
    int expected_ = -1;

  public:
    void clear() {
        buffer_.clear();
        expected_ = -1;
    }

    static QByteArray frame(const QJsonObject& message);
    bool feed(const QByteArray& bytes, const std::function<void(const QJsonObject&)>& receive,
              QString& error);
};
