#include <QCoreApplication>
#include <QThread>
#include <iostream>
#include "extraction/resource_limits.h"
#ifdef _WIN32
#include <windows.h>
#endif
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const auto mode = app.arguments().value(1);
    if (mode == "memory") {
        filesearcher::ParserLimits limits;
        if (!limits.ready()) return 3;
        auto* allocation = VirtualAlloc(nullptr, 512ULL * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (allocation) { VirtualFree(allocation, 0, MEM_RELEASE); return 4; }
        std::cout << R"({"status":"memory_limited"})";
        return 0;
    }
#ifdef _WIN32
    if (mode == "crash") TerminateProcess(GetCurrentProcess(), 0xC0000005);
#endif
    if (mode == "hang") QThread::sleep(30);
    if (mode == "fail") return 17;
    if (mode == "malformed") { std::cout << "invalid json"; return 0; }
    std::cout << R"({"status":"ok","text":"Synthetic extraction fixture"})";
    return 0;
}
