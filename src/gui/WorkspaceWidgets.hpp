#pragma once

#include <QLabel>
#include <QSizePolicy>

namespace tsui::widgets {
inline QLabel* label(const QString& text, const QString& cls = "") {
    auto* l = new QLabel(text);
    if (!cls.isEmpty())
        l->setProperty("class", cls);
    l->setWordWrap(true);
    l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    return l;
}
}
