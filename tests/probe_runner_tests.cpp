#include "extraction/probe_runner.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <iostream>
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) return 2;
    const auto worker = app.arguments()[1];
    if (filesearcher::runExtractionProbe(worker, "memory").status != "memory_limited") return 1;
    const auto success = filesearcher::runExtractionProbe(worker, "ok");
    if (success.status != "ok" || success.text != "Synthetic extraction fixture") return 1;
    if (filesearcher::runExtractionProbe(worker, "fail").status != "worker_failed") return 1;
#ifdef _WIN32
    if (filesearcher::runExtractionProbe(worker, "crash").status != "worker_failed") return 1;
#endif
    if (filesearcher::runExtractionProbe(worker, "malformed").status != "invalid_response") return 1;
    QElapsedTimer timer;
    timer.start();
    if (filesearcher::runExtractionProbe(worker, "hang", 300).status != "timeout") return 1;
    if (timer.elapsed() > 3000) return 1;
    if (filesearcher::runExtractionProbe(worker, "ok").status != "ok") return 1;
    std::cout << "Worker failure, timeout and recovery checks passed\n";
    return 0;
}
