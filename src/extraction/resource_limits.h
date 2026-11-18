#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace filesearcher {
class ParserLimits {
    HANDLE job_ = nullptr;
public:
    explicit ParserLimits(SIZE_T memoryBytes = 256 * 1024 * 1024) {
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = memoryBytes;
        if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job_, GetCurrentProcess())) {
            if (job_) CloseHandle(job_);
            job_ = nullptr;
        }
    }
    ~ParserLimits() { if (job_) CloseHandle(job_); }
    bool ready() const { return job_ != nullptr; }
    ParserLimits(const ParserLimits&) = delete;
    ParserLimits& operator=(const ParserLimits&) = delete;
};
}
#endif
