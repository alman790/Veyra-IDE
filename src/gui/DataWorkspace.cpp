#include "DatabasePanel.hpp"
#include "MainWindow.hpp"
#include "UiLanguage.hpp"
#include <QDockWidget>
#include <QPushButton>
#include <QTabWidget>

void MainWindow::setupDataTools() {
    databaseDock_ = new QDockWidget(UiLanguage::tr("Database"), this);
    databaseDock_->setObjectName("databaseDock");
    databaseDock_->setMinimumHeight(350);
    databaseTabs_ = new QTabWidget(databaseDock_);
    databaseTabs_->setObjectName("databaseConnections");
    databaseTabs_->setTabsClosable(true);
    auto* add = new QPushButton("+");
    add->setToolTip(UiLanguage::tr("New database connection"));
    databaseTabs_->setCornerWidget(add);
    auto create = [this] {
        auto* panel = new DatabasePanel(databaseTabs_);
        databaseTabs_->addTab(panel, UiLanguage::tr("Connection"));
        databaseTabs_->setCurrentWidget(panel);
        connect(panel, &DatabasePanel::titleChanged, this, [this, panel](const QString& title) {
            const int index = databaseTabs_->indexOf(panel);
            if (index >= 0)
                databaseTabs_->setTabText(index, title);
        });
        return panel;
    };
    create();
    connect(add, &QPushButton::clicked, this, [create] { create(); });
    connect(databaseTabs_, &QTabWidget::tabCloseRequested, this, [this, create](int index) {
        auto* panel = qobject_cast<DatabasePanel*>(databaseTabs_->widget(index));
        if (!panel || !panel->requestClose())
            return;
        databaseTabs_->removeTab(index);
        panel->deleteLater();
        if (databaseTabs_->count() == 0)
            create();
    });
    databaseDock_->setWidget(databaseTabs_);
    addDockWidget(Qt::BottomDockWidgetArea, databaseDock_);
    if (outputDock_)
        tabifyDockWidget(outputDock_, databaseDock_);
    databaseDock_->hide();
}
