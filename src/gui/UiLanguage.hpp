#pragma once
#include <QString>
class QWidget;

namespace UiLanguage {
void initialize();
QString code();
QString nativeName(const QString& code);
void setLanguage(const QString& code);
QString text(const QString& source);

inline QString tr(const char* source) {
    return text(QString::fromUtf8(source));
}

void apply(QWidget* root = nullptr);
}
