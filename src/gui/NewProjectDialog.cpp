#include "../application/ProjectCreator.hpp"
#include "MainWindow.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>

void MainWindow::createProject() {
    QDialog dialog(this);
    dialog.setObjectName("newProjectDialog");
    dialog.setWindowTitle(UiLanguage::tr("New project"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit("MyProject");
    name->setObjectName("newProjectName");
    form->addRow(UiLanguage::tr("Title"), name);
    auto* location = new QLineEdit(QDir::homePath());
    location->setObjectName("newProjectLocation");
    auto* browse = new QPushButton("…");
    auto* directory = new QHBoxLayout;
    directory->addWidget(location);
    directory->addWidget(browse);
    form->addRow(UiLanguage::tr("Directory"), directory);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto path =
            QFileDialog::getExistingDirectory(&dialog, UiLanguage::tr("Project directory"), location->text());
        if (!path.isEmpty())
            location->setText(path);
    });
    auto* language = new QComboBox;
    language->setObjectName("newProjectLanguage");
    for (const auto& profile : ts::availableLanguages())
        if (profile.runnable)
            language->addItem(QString::fromStdString(profile.name), int(profile.id));
    form->addRow(UiLanguage::tr("Language"), language);
    auto* discover = new QPushButton(UiLanguage::tr("Discover installed SDKs…"));
    form->addRow(discover);
    auto* sdk = new QComboBox;
    sdk->setObjectName("newProjectSdk");
    sdk->addItem(UiLanguage::tr("System tools"), QJsonObject{});
    for (const auto& value : ToolDiscovery::sdks())
        sdk->addItem(value.name + " · " + value.root,
                     QJsonObject{{"name", value.name}, {"root", value.root}, {"variable", value.variable}});
    form->addRow("SDK", sdk);
    connect(discover, &QPushButton::clicked, &dialog, [sdk, language] {
        for (const auto& item : ToolDiscovery::detect())
            if (int(item.language.id) == language->currentData().toInt())
                sdk->addItem(QString::fromStdString(item.language.name) + " · " + item.root,
                             QJsonObject{{"name", QString::fromStdString(item.language.name)},
                                         {"root", item.root},
                                         {"variable", QString::fromStdString(item.language.sdkVariable)}});
        if (sdk->count() > 1) {
            sdk->setCurrentIndex(sdk->count() - 1);
            sdk->showPopup();
        }
    });
    auto* sdkPath = new QLineEdit;
    sdkPath->setObjectName("newProjectSdkRoot");
    sdkPath->setPlaceholderText(UiLanguage::tr("Or enter a separate SDK directory"));
    form->addRow(UiLanguage::tr("SDK directory"), sdkPath);
    auto* note =
        new QLabel(UiLanguage::tr("Type and library completions come from a local language server using the "
                                  "selected SDK environment. Configure the server in Languages and SDKs."));
    note->setWordWrap(true);
    form->addRow(note);
    auto* error = new QLabel;
    error->setObjectName("newProjectError");
    error->setWordWrap(true);
    form->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        auto chosen = sdk->currentData().toJsonObject();
        if (!sdkPath->text().trimmed().isEmpty())
            chosen = QJsonObject{{"name", "Project SDK"}, {"root", sdkPath->text().trimmed()}};
        const auto id = static_cast<ts::LanguageId>(language->currentData().toInt());
        if (!chosen.value("root").toString().isEmpty() && chosen.value("variable").toString().isEmpty())
            chosen["variable"] = QString::fromStdString(ts::profileFor(id).sdkVariable);
        QString root, entry, issue;
        if (!ProjectCreator::create({location->text(), name->text().trimmed(), id, chosen}, root, entry,
                                    issue)) {
            error->setText(UiLanguage::text(issue));
            return;
        }
        workspaceRoot_ = root;
        QSettings().setValue("workspace/root", root);
        updateProjectTree();
        loadCodeFile(entry);
        syncLanguage(true);
        dialog.accept();
    });
    dialog.resize(640, 340);
    name->selectAll();
    name->setFocus();
    dialog.exec();
}
