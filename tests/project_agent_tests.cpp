#include "../src/application/AgentContext.hpp"
#include "AgentPanel.hpp"
#include "MainWindow.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

static bool waitFor(const std::function<bool()>& predicate, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout)
        QTest::qWait(10);
    return predicate();
}

static void write(const QString& path, const QByteArray& text) {
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "Write agent project fixture");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("ProjectAgent");
    QTemporaryDir settings, project, outside;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        check(argc == 2, "Agent transport fixture is required");
        write(project.filePath("Counter.hpp"), "struct Counter { int value = 7; int next() const; };\n");
        write(project.filePath("Counter.cpp"),
              "#include \"Counter.hpp\"\nint Counter::next() const { return missing; }\n");
        write(project.filePath(".env"), "private_token=do-not-include\n");
        write(project.filePath(".gitignore"), "/generated.cpp\n");
        write(project.filePath("generated.cpp"), "generated source outside agent context\n");
        write(outside.filePath("outside.cpp"), "private outside source\n");
#ifdef Q_OS_WIN
        const auto externalLink = QStringLiteral("outside-link.cpp.lnk");
#else
        const auto externalLink = QStringLiteral("outside-link.cpp");
#endif
        check(QFile::link(outside.filePath("outside.cpp"), project.filePath(externalLink)),
              "Create a valid external link fixture for the current platform");
        AgentInput input;
        input.root = project.path();
        input.path = project.filePath("Counter.cpp");
        input.text = "#include \"Counter.hpp\"\nint Counter::next() const { return missing; }\n";
        input.cursor = input.text.indexOf("missing");
        input.language = "C++";
        input.sdk = "test SDK";
        input.buildOutput = "Counter.cpp:2: error: use of undeclared identifier 'missing'";
        auto context = AgentContext::collect(input, [] { return false; });
        check(context.prompt().contains("Counter.hpp") && context.prompt().contains("int value = 7") &&
                  context.prompt().contains("undeclared identifier") && context.prompt().contains("test SDK"),
              "Context contains neighboring classes, actual diagnostics and SDK information");
        check(!context.files().contains(".env"), "Context omits secret files");
        check(!context.files().contains(externalLink), "Context omits external project links");
        check(!context.files().contains("generated.cpp") &&
                  context.read("generated.cpp").startsWith("Error:"),
              "Project ignore rules govern both context enumeration and explicit agent reads");
        check(context.read("../outside.cpp").startsWith("Error:") &&
                  context.read(".env").startsWith("Error:") &&
                  context.read(externalLink).startsWith("Error:"),
              "Context reads stay inside the project");
        check(context.search("Counter::next", [] { return false; }).contains("Counter.cpp:2"),
              "Project symbol search returns source locations");
        check(context.read("Counter.cpp", 2).contains("from line 2)\nint Counter::next"),
              "A line-based lookup includes the requested line rather than skipping it");
        auto distantInput = input;
        distantInput.text.clear();
        distantInput.cursor = 0;
        for (int line = 0; line < 1000; ++line)
            distantInput.text += QString("int variable_%1 = %1;\n").arg(line);
        auto distant = AgentContext::collect(distantInput, [] { return false; });
        distant.read("Counter.cpp", 900);
        check(distant.editGrammar().find("variable_899") != std::string::npos,
              "Grounded edits can target code explicitly read far from the original cursor");
        AgentProposal proposal;
        QString error;
        const auto replacement = [](const QString& path, const QString& before, const QString& after) {
            return QJsonObject{
                {"action", "propose"},
                {"summary", "Fix the unresolved member reference."},
                {"edits", QJsonArray{QJsonObject{{"path", path}, {"before", before}, {"after", after}}}}};
        };
        check(
            context.proposal(replacement("Counter.cpp", "return missing;", "return value;"), proposal, error),
            "Exact source replacement is accepted");
        check(!context.proposal(replacement("Counter.cpp", "nonexistent code", "return value;"), proposal,
                                error),
              "Invented source text cannot become an edit");
        check(!context.proposal(replacement("../outside.cpp", "private", "changed"), proposal, error),
              "Proposals cannot escape the workspace");
        const QJsonObject overlap{
            {"action", "propose"},
            {"summary", "Invalid overlapping changes"},
            {"edits",
             QJsonArray{QJsonObject{{"path", "Counter.cpp"},
                                    {"before", "return missing;"},
                                    {"after", "return value;"}},
                        QJsonObject{{"path", "Counter.cpp"}, {"before", "missing"}, {"after", "value"}}}}};
        check(!context.proposal(overlap, proposal, error), "Overlapping model replacements are rejected");
        auto repeatedInput = input;
        repeatedInput.text += "// return missing;\n";
        const auto repeated = AgentContext::collect(repeatedInput, [] { return false; });
        check(!repeated.proposal(replacement("Counter.cpp", "return missing;", "return value;"), proposal,
                                 error),
              "Ambiguous source matches cannot become edits");
        auto unsavedInput = input;
        unsavedInput.openFiles[project.filePath("Counter.hpp")] =
            "struct Counter { int value = 99; int next() const; };\n";
        const auto unsaved = AgentContext::collect(unsavedInput, [] { return false; });
        check(unsaved.prompt().contains("int value = 99") && !unsaved.prompt().contains("int value = 7"),
              "Related class context prefers unsaved buffers over disk contents");
        auto typoInput = input;
        typoInput.text.replace("missing", "vlaue");
        typoInput.buildOutput = "error: undeclared identifier 'vlaue'; did you mean 'value'?";
        const auto typo = AgentContext::collect(typoInput, [] { return false; });
        const auto repairs = typo.identifierRepairs();
        check(!repairs.isEmpty() && repairs.first().after.contains("return value;"),
              "Identifier repairs are drawn from the actual neighboring declaration");
        typoInput.text.replace("vlaue", "value");
        typoInput.buildOutput = "error: type mismatch for 'value'";
        const auto declared = AgentContext::collect(typoInput, [] { return false; });
        check(declared.identifierRepairs().isEmpty(),
              "A declared identifier is not treated as a spelling error");

        QSettings().setValue("agent/beamcoreExecutable", QString::fromLocal8Bit(argv[1]));
        QSettings().setValue("workspace/root", project.path());
        QSettings().setValue("lsp/enabled", false);
        MainWindow window;
        window.show();
        auto* editor = window.findChild<CodeEditor*>("codeEditor");
        editor->openFileRequested(input.path);
        auto originalCaret = editor->textCursor();
        originalCaret.setPosition(editor->toPlainText().lastIndexOf('}'));
        editor->setTextCursor(originalCaret);
        auto* panel = window.findChild<AgentPanel*>("projectAgentPanel");
        check(panel, "Project agent dock is installed");
        window.findChild<QDockWidget*>("projectAgentDock")->show();
        check(waitFor([&] { return panel->findChild<QComboBox*>("agentProvider")->count() == 2; }),
              "Provider list arrives through the Beamcore transport");
        bool providerSettings = false;
        QTimer::singleShot(0, panel, [&] {
            if (auto* dialog = panel->findChild<QDialog*>("agentProviderSettings")) {
                const auto* key = dialog->findChild<QLineEdit*>("providerSettingsKey");
                providerSettings = dialog->findChild<QLineEdit*>("providerSettingsEndpoint") &&
                                   dialog->findChild<QLineEdit*>("providerSettingsModel") && key &&
                                   key->echoMode() == QLineEdit::Password;
                dialog->reject();
            }
        });
        panel->findChild<QPushButton*>("agentConfigureProviders")->click();
        check(providerSettings, "API endpoint, model and protected key are configurable inside the IDE");
        panel->findChild<QComboBox*>("agentProvider")->setCurrentIndex(1);
        const auto send = [&] {
            panel->findChild<QPlainTextEdit*>("agentQuestion")
                ->setPlainText("Fix missing and set the initial value to 8.");
            panel->findChild<QPushButton*>("agentProposeFix")->click();
            return waitFor([&] { return !panel->agent()->busy(); });
        };
        check(send() && panel->proposal().edits.size() == 2,
              "Agent reads related code, searches symbols, then proposes a multi-file fix");
        check(
            panel->findChild<QPlainTextEdit*>("agentPatchPreview")->toPlainText().contains("+return value;"),
            "Before/after changes are visible before application");
        check(editor->toPlainText().contains("return missing;"),
              "Generating a proposal does not change source text");
        if (qEnvironmentVariableIsSet("TS_AGENT_PREVIEW")) {
            window.resize(1500, 900);
            app.processEvents();
            window.grab().save(qEnvironmentVariable("TS_AGENT_PREVIEW"));
        }
        panel->findChild<QPushButton*>("agentApplyProposal")->click();
        check(editor->toPlainText().contains("return value;"),
              "Accept applies the requested source replacement");
        check(editor->document()->characterAt(editor->textCursor().position()) == '}',
              "Multi-file edits preserve the caret's semantic position when earlier text changes length");
        QFile unchanged(input.path);
        check(unchanged.open(QIODevice::ReadOnly) && unchanged.readAll().contains("return missing;"),
              "Applying a proposal changes buffers without silently saving to disk");
        editor->undo();
        check(editor->toPlainText().contains("return missing;"),
              "Applied changes undo as a single document operation");
        editor->openFileRequested(project.filePath("Counter.hpp"));
        auto* headerDocument = editor->document();
        editor->undo();
        editor->openFileRequested(input.path);

        panel->agent()->clearHistory();
        check(waitFor([&] { return !panel->agent()->beamcore()->running(); }),
              "Previous provider process stops before a new discussion");
        check(send() && !panel->proposal().edits.isEmpty(), "A new discussion collects fresh context");
        auto header = QTextCursor(headerDocument);
        header.select(QTextCursor::Document);
        header.insertText("struct Counter { int value = 99; int next() const; };\n");
        check(!panel->proposal().edits.isEmpty(),
              "Changing an inactive buffer exercises application-time validation");
        panel->findChild<QPushButton*>("agentApplyProposal")->click();
        check(editor->toPlainText().contains("return missing;"),
              "A changed companion buffer prevents all proposal edits");
        qputenv("VEYRA_AGENT_FIXTURE", "invalid");
        panel->agent()->cancel();
        check(waitFor([&] { return !panel->agent()->beamcore()->running(); }),
              "Provider stops before the invalid-response case");
        int failures = 0;
        QObject::connect(panel->agent(), &ProjectAgent::failed, panel, [&](const QString&) { ++failures; });
        panel->agent()->start(input, "Review this code.", "test-provider", "fixture-model");
        check(waitFor([&] { return !panel->agent()->busy(); }) && failures == 1 &&
                  panel->proposal().edits.isEmpty(),
              "Invalid model output is rejected after one bounded correction attempt");
        qputenv("VEYRA_AGENT_FIXTURE", "wait");
        panel->agent()->cancel();
        check(waitFor([&] { return !panel->agent()->beamcore()->running(); }),
              "Provider stops before the cancellation case");
        panel->agent()->start(input, "Review this code.", "test-provider", "fixture-model");
        QTest::qWait(50);
        panel->agent()->cancel();
        check(waitFor([&] { return !panel->agent()->busy(); }),
              "Cancellation stops an outstanding provider request");
        qunsetenv("VEYRA_AGENT_FIXTURE");
        for (auto* document : window.findChildren<QTextDocument*>())
            document->setModified(false);
        editor->document()->setModified(false);
        window.hide();
        std::cout << "PASS: context, provider bridge, lookup loop, reviewed edits, atomic validation and "
                     "cancellation\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
