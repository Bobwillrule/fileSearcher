#include "extraction/probe_runner.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <iostream>
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    const auto result = filesearcher::runExtractionProbe(app.arguments()[1], app.arguments()[2]);
    if (result.status != "ok" || !result.text.contains("Master Theorem")) {
        std::cerr << "Text filter did not return the expected fixture content\n";
        return 1;
    }
    const auto missing = QFileInfo(app.arguments()[2]).absolutePath() + "/nonexistent-fixture-43f96.txt";
    if (filesearcher::runExtractionProbe(app.arguments()[1], missing).status != "missing_file") return 1;
    return 0;
}
