#include "ProcessTree.hpp"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
ProcessTree::~ProcessTree() {
    reset();
}

void ProcessTree::configure(QProcess& process) {
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NEW_PROCESS_GROUP; });
#else
    process.setChildProcessModifier([] {
        if (setsid() < 0)
            _exit(126);
    });
#endif
}

bool ProcessTree::attach(qint64 pid, QString& error) {
    reset();
    pid_ = pid;
    error.clear();
#ifdef Q_OS_WIN
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job)
            CloseHandle(job);
        error = "Cannot create process job";
        return false;
    }
    HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    const bool attached = child && AssignProcessToJobObject(job, child);
    if (child)
        CloseHandle(child);
    if (!attached) {
        CloseHandle(job);
        error = "Cannot assign child process to termination job";
        return false;
    }
    job_ = job;
#endif
    return true;
}

void ProcessTree::terminate() {
#ifdef Q_OS_WIN
    if (job_)
        TerminateJobObject(static_cast<HANDLE>(job_), 130);
#else
    if (pid_ > 0)
        kill(-static_cast<pid_t>(pid_), SIGKILL);
#endif
}

void ProcessTree::reset() {
    terminate();
#ifdef Q_OS_WIN
    if (job_)
        CloseHandle(static_cast<HANDLE>(job_));
#endif
    job_ = nullptr;
    pid_ = 0;
}
