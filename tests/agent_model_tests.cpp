#include "ProjectAgent.hpp"
#include "UiLanguage.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VeyraTests");
    app.setApplicationName("AgentModel");
    QTemporaryDir project, settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings().setValue("agent/beamcoreConfig", settings.filePath("isolated-config.dets"));
    qunsetenv("TS_DISABLE_LOCAL_MODEL");
    try {
        UiLanguage::initialize();
        const QByteArray source = "#include \"Counter.hpp\"\nint Counter::next() const { return vlaue; }\n";
        QFile implementation(project.filePath("Counter.cpp")), header(project.filePath("Counter.hpp"));
        if (!implementation.open(QIODevice::WriteOnly) || !header.open(QIODevice::WriteOnly))
            throw std::runtime_error("Create model context fixture");
        implementation.write(source);
        header.write("struct Counter { int value = 7; int next() const; };\n");
        implementation.close();
        header.close();
        AgentInput input;
        input.root = project.path();
        input.path = implementation.fileName();
        input.text = QString::fromUtf8(source);
        input.cursor = input.text.indexOf("vlaue");
        input.language = "C++";
        input.requestChanges = true;
        input.sdk = "C++ SDK";
        const auto compile = [&] {
            QProcess compiler;
            compiler.setWorkingDirectory(project.path());
            compiler.setProcessChannelMode(QProcess::MergedChannels);
#ifdef _MSC_VER
            const QStringList arguments{"/nologo", "/std:c++20", "/Zs", "Counter.cpp"};
#else
            const QStringList arguments{"-std=c++20", "-fsyntax-only", "Counter.cpp"};
#endif
            compiler.start(TS_TEST_CXX_COMPILER, arguments);
            if (!compiler.waitForFinished(20000) || compiler.exitStatus() != QProcess::NormalExit)
                throw std::runtime_error("Run the configured C++ compiler for model verification");
            return qMakePair(compiler.exitCode(), QString::fromLocal8Bit(compiler.readAll()));
        };
        const auto diagnostic = compile();
        if (diagnostic.first == 0)
            throw std::runtime_error("The original model fixture must contain a compiler error");
        input.buildOutput = diagnostic.second;
        ProjectAgent agent;
        AgentProposal proposal;
        QString error, answer, rawResponse;
        QObject::connect(agent.beamcore(), &BeamcoreClient::responseReceived, &agent,
                         [&](const QString& value) { rawResponse = value; });
        QObject::connect(&agent, &ProjectAgent::proposalReady, &agent,
                         [&](const AgentProposal& value, const QString&) { proposal = value; });
        QObject::connect(&agent, &ProjectAgent::failed, &agent, [&](const QString& value) { error = value; });
        QObject::connect(&agent, &ProjectAgent::answerReady, &agent,
                         [&](const QString& value) { answer = value; });
        agent.start(
            input,
            "Fix the misspelled identifier in the return expression. Keep the class interface unchanged.");
        QElapsedTimer deadline;
        deadline.start();
        while (agent.busy() && deadline.elapsed() < 120000)
            QTest::qWait(20);
        if (agent.busy()) {
            agent.cancel();
            throw std::runtime_error("Real project-aware local model exceeded its test deadline");
        }
        if (!error.isEmpty() || proposal.edits.isEmpty())
            throw std::runtime_error(
                ("Expected a real local model repair proposal: " + error + " " + answer + " " + rawResponse)
                    .toStdString());
        QString repaired = input.text;
        for (const auto& edit : proposal.edits)
            if (edit.path == "Counter.cpp")
                repaired.replace(edit.offset, edit.before.size(), edit.after);
        const QRegularExpression memberReturn(R"(return\s+(?:(?:this\s*->|Counter::)\s*)?value\s*;)");
        if (!memberReturn.match(repaired).hasMatch() || repaired.contains("return vlaue;"))
            throw std::runtime_error(
                "Local agent must resolve the actual missing-member diagnostic using the neighboring class");
        if (!implementation.open(QIODevice::WriteOnly) || implementation.write(repaired.toUtf8()) < 0)
            throw std::runtime_error("Write repaired verification fixture");
        implementation.close();
        if (compile().first != 0)
            throw std::runtime_error("The real compiler must accept Qwen's proposed repair");
        agent.clearHistory();
        proposal = {};
        input.text = repaired;
        input.requestChanges = false;
        agent.start(input, "Explain the selected method and its class.");
        QTest::qWait(100);
        agent.cancel();
        QElapsedTimer cancellation;
        cancellation.start();
        while (agent.busy() && cancellation.elapsed() < 10000)
            QTest::qWait(10);
        QTest::qWait(100);
        if (agent.busy() || !proposal.edits.isEmpty())
            throw std::runtime_error("Cancelled native inference must not publish a late proposal");
        std::cout << "PASS: bundled Qwen proposes a grounded cross-file diagnostic repair in "
                  << deadline.elapsed() << " ms\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
