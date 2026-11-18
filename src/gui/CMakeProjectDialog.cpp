#include "CMakeProjectDialog.hpp"
#include "ToolDiscovery.hpp"
#include "UiLanguage.hpp"
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString key(const QString& root) {
    return "cmake/project/" +
           QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex());
}
}

CMakeProjectDialog::CMakeProjectDialog(const QString& root, std::function<void(const QString&)> output,
                                       QWidget* parent, ProjectAction action)
    : QDialog(parent), output_(std::move(output)), initialAction_(action) {
    setWindowTitle(UiLanguage::tr("CMake project · Configure / Build / Run"));
    resize(740, 400);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    root_ = new QLineEdit(root);
    root_->setReadOnly(true);
    build_ = new QLineEdit;
    generator_ = new QComboBox;
    generator_->setEditable(true);
    generator_->addItem("");
    configuration_ = new QComboBox;
    configuration_->addItems({"Debug", "Release", "RelWithDebInfo", "MinSizeRel"});
    target_ = new QComboBox;
    target_->setEnabled(false);
    const auto prefix = key(root);
    QSettings prefs;
    configuration_->setCurrentText(prefs.value(prefix + "/configuration", "Debug").toString());
    generator_->setCurrentText(prefs.value(prefix + "/generator").toString());
    build_->setText(prefs
                        .value(prefix + "/build",
                               QDir(root).filePath(".veyra/cmake-" + configuration_->currentText().toLower()))
                        .toString());
    auto* input = new QLineEdit;
    input->setPlaceholderText(UiLanguage::tr("Program stdin · Enter"));
    input->setEnabled(false);
    connect(input, &QLineEdit::returnPressed, this, [this, input] {
        runner_.sendInput(input->text());
        input->clear();
    });
    connect(&runner_, &CodeRunner::stateChanged, this,
            [input](bool active, bool program) { input->setEnabled(active && program); });
    form->addRow(UiLanguage::tr("Project"), root_);
    form->addRow(UiLanguage::tr("Build directory"), build_);
    form->addRow(UiLanguage::tr("Generator (empty — default)"), generator_);
    form->addRow(UiLanguage::tr("Configuration"), configuration_);
    form->addRow(UiLanguage::tr("CMake File API target"), target_);
    form->addRow(UiLanguage::tr("Program input"), input);
    layout->addLayout(form);
    auto* buttons = new QHBoxLayout;
    configure_ = new QPushButton("Configure");
    compile_ = new QPushButton("Build");
    run_ = new QPushButton("Run");
    stop_ = new QPushButton("Stop");
    for (auto* button : {configure_, compile_, run_, stop_})
        buttons->addWidget(button);
    layout->addLayout(buttons);
    compile_->setEnabled(false);
    run_->setEnabled(false);
    stop_->setEnabled(false);
    status_ = new QLabel(
        UiLanguage::tr("Configure retrieves actual targets and executable paths from the CMake File API."));
    status_->setObjectName("cmakeStatus");
    status_->setWordWrap(true);
    layout->addWidget(status_);
    layout->addStretch();
    connect(configure_, &QPushButton::clicked, this, &CMakeProjectDialog::configureProject);
    connect(compile_, &QPushButton::clicked, this, [this] { buildProject(false); });
    connect(run_, &QPushButton::clicked, this, [this] { buildProject(true); });
    connect(stop_, &QPushButton::clicked, &runner_, &CodeRunner::stop);
    connect(target_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        run_->setEnabled(!runner_.busy() && index >= 0 && index < targets_.size() &&
                         !targets_[index].artifact.isEmpty());
    });
    connect(configuration_, &QComboBox::currentTextChanged, this, [this, root](const QString& value) {
        build_->setText(QDir(root).filePath(".veyra/cmake-" + value.toLower()));
        targets_.clear();
        target_->clear();
        compile_->setEnabled(false);
        run_->setEnabled(false);
    });
    const auto invalidate = [this] {
        if (runner_.busy())
            return;
        targets_.clear();
        target_->clear();
        target_->setEnabled(false);
        compile_->setEnabled(false);
        run_->setEnabled(false);
        status_->setText(UiLanguage::tr("Settings changed. Run Configure."));
    };
    connect(build_, &QLineEdit::textChanged, this, invalidate);
    connect(generator_, &QComboBox::currentTextChanged, this, invalidate);
    connect(&capabilities_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                if (code != 0)
                    return;
                const auto object = QJsonDocument::fromJson(capabilities_.readAllStandardOutput()).object();
                const auto selected = generator_->currentText();
                for (const auto& value : object.value("generators").toArray()) {
                    const auto name = value.toObject().value("name").toString();
                    if (generator_->findText(name) < 0)
                        generator_->addItem(name);
                }
                generator_->setCurrentText(selected);
            });
    capabilities_.setProcessEnvironment(ToolDiscovery::environment());
    const auto cmake = prefs.value("toolchains/cmake", "cmake").toString();
    capabilities_.start(ToolDiscovery::find(cmake).isEmpty() ? cmake : ToolDiscovery::find(cmake),
                        {"-E", "capabilities"});
    QTimer::singleShot(0, this, [this] { configureProject(); });
}

CMakeProjectDialog::~CMakeProjectDialog() {
    if (capabilities_.state() != QProcess::NotRunning) {
        capabilities_.kill();
        capabilities_.waitForFinished(300);
    }
}

CMakeSettings CMakeProjectDialog::settings() const {
    return {root_->text(), QDir(build_->text()).absolutePath(), generator_->currentText(),
            configuration_->currentText(), target_->currentText()};
}

void CMakeProjectDialog::setBusy(bool busy) {
    configure_->setEnabled(!busy);
    root_->setEnabled(!busy);
    build_->setEnabled(!busy);
    generator_->setEnabled(!busy);
    configuration_->setEnabled(!busy);
    target_->setEnabled(!busy && !targets_.isEmpty());
    compile_->setEnabled(!busy && !targets_.isEmpty());
    const int index = target_->currentIndex();
    run_->setEnabled(!busy && index >= 0 && index < targets_.size() && !targets_[index].artifact.isEmpty());
    stop_->setEnabled(busy);
}

void CMakeProjectDialog::configureProject() {
    if (runner_.busy())
        return;
    QString error;
    const auto options = settings();
    if (!CMakeFileApi::prepareQuery(options.build, error)) {
        status_->setText(UiLanguage::text(error));
        return;
    }
    ts::Array<ts::ProcessSpec> commands;
    commands.pushBack(CMakeFileApi::configureCommand(options));
    setBusy(true);
    status_->setText("CMake configure…");
    if (!runner_.startCommands(
            std::move(commands), false, output_,
            [this, options](bool success, int code) {
                if (closing_) {
                    QDialog::reject();
                    return;
                }
                setBusy(false);
                if (!success) {
                    status_->setText(
                        QString(UiLanguage::tr("Configure failed with exit code %1. See the console."))
                            .arg(code));
                    return;
                }
                QString error;
                targets_ = CMakeFileApi::readTargets(options.build, options.configuration, error);
                target_->clear();
                for (const auto& target : targets_)
                    target_->addItem(target.name + " [" + target.type + "]");
                const auto remembered = QSettings().value(key(options.root) + "/target").toString();
                int selected = -1;
                for (int index = 0; index < targets_.size(); ++index)
                    if (targets_[index].name == remembered) {
                        selected = index;
                        break;
                    }
                if (selected < 0)
                    for (int index = 0; index < targets_.size(); ++index)
                        if (!targets_[index].artifact.isEmpty()) {
                            selected = index;
                            break;
                        }
                if (selected >= 0)
                    target_->setCurrentIndex(selected);
                QSettings prefs;
                const auto prefix = key(options.root);
                prefs.setValue(prefix + "/configuration", options.configuration);
                prefs.setValue(prefix + "/generator", options.generator);
                prefs.setValue(prefix + "/build", options.build);
                status_->setText(
                    error.isEmpty()
                        ? QString(UiLanguage::tr("Found %1 targets. Select one and press Build or Run."))
                              .arg(targets_.size())
                        : error);
                setBusy(false);
                const auto action = initialAction_;
                initialAction_ = ProjectAction::Configure;
                if (error.isEmpty() && action != ProjectAction::Configure)
                    QTimer::singleShot(0, this,
                                       [this, action] { buildProject(action == ProjectAction::Run); });
            },
            error)) {
        setBusy(false);
        status_->setText(UiLanguage::text(error));
    }
}

void CMakeProjectDialog::buildProject(bool execute) {
    const int index = target_->currentIndex();
    if (runner_.busy() || index < 0 || index >= targets_.size())
        return;
    const auto options = settings();
    const auto target = targets_[index];
    QSettings().setValue(key(options.root) + "/target", target.name);
    if (execute && target.artifact.isEmpty()) {
        status_->setText(UiLanguage::tr("The selected target is not executable."));
        return;
    }
    ts::Array<ts::ProcessSpec> commands;
    commands.pushBack(CMakeFileApi::buildCommand(options, target.name));
    if (execute)
        commands.pushBack(CMakeFileApi::runCommand(options, target));
    QString error;
    setBusy(true);
    status_->setText(execute ? "Build → Run: " + target.artifact : "Build: " + target.name);
    if (!runner_.startCommands(
            std::move(commands), execute, output_,
            [this](bool success, int code) {
                if (closing_) {
                    QDialog::reject();
                    return;
                }
                setBusy(false);
                status_->setText(
                    success ? UiLanguage::tr("Operation completed successfully.")
                            : QString(UiLanguage::tr("Operation stopped or failed with exit code %1."))
                                  .arg(code));
            },
            error)) {
        setBusy(false);
        status_->setText(UiLanguage::text(error));
    }
}

void CMakeProjectDialog::reject() {
    if (runner_.busy()) {
        closing_ = true;
        runner_.stop();
        status_->setText(UiLanguage::tr("Stopping process tree…"));
        return;
    }
    QDialog::reject();
}
