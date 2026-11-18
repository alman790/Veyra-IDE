#pragma once
#include "UiIcons.hpp"
#include <QFileSystemModel>
#include <QSortFilterProxyModel>

class ProjectFileFilter final : public QSortFilterProxyModel {
    bool focused_ = true;
    QString query_, active_;

    template <typename Change> void changeFilter(Change change) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        change();
        endFilterChange(Direction::Rows);
#else
        change();
        invalidateFilter();
#endif
    }

  public:
    void setFocused(bool value) {
        changeFilter([&] { focused_ = value; });
    }

    void setQuery(const QString& value) {
        changeFilter([&] { query_ = value; });
    }

    void setActivePath(const QString& value) {
        if (active_ != value)
            changeFilter([&] { active_ = value; });
    }

    using QSortFilterProxyModel::QSortFilterProxyModel;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (role == Qt::DecorationRole && index.column() == 0) {
            auto* files = qobject_cast<QFileSystemModel*>(sourceModel());
            const auto source = mapToSource(index);
            if (files) {
                if (files->isDir(source))
                    return tsui::icon("folder", QColor("#91A8BF"));
                const auto name = files->fileName(source);
                return tsui::icon(name.endsWith(".cpp") || name.endsWith(".c") ? "code" : "file",
                                  name.endsWith(".hpp") || name.endsWith(".h") ? QColor("#C9B271")
                                                                               : QColor("#8BA9CE"));
            }
        }
        return QSortFilterProxyModel::data(index, role);
    }

  protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        auto* files = qobject_cast<QFileSystemModel*>(sourceModel());
        if (files && files->isDir(left) != files->isDir(right))
            return files->isDir(left);
        return QSortFilterProxyModel::lessThan(left, right);
    }

    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        auto* files = qobject_cast<QFileSystemModel*>(sourceModel());
        if (!files)
            return false;
        const auto index = files->index(row, 0, parent);
        const auto name = files->fileName(index);
        const auto path = files->filePath(index), root = files->rootPath();
        if (path == root || root.startsWith(path + '/'))
            return true;
        if (path == active_ || active_.startsWith(path + '/'))
            return true;
        if (focused_ && !files->isDir(index) && name.endsWith(".log"))
            return false;
        if (!query_.isEmpty() && !files->isDir(index) && !name.contains(query_, Qt::CaseInsensitive))
            return false;
        if (focused_ && files->isDir(index) &&
            (name == ".git" || name == ".idea" || name == ".veyra" || name == "node_modules" ||
             name == "__pycache__" || name == ".venv" || name == "build" || name.startsWith("build-") ||
             name.startsWith("cmake-build")))
            return false;
        return true;
    }
};
