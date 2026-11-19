#pragma once
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <atomic>
#include <memory>
#include <vector>
#define NOMINMAX
#include <windows.h>

namespace filesearcher::cleanup {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v):value(v) {}
    ~Handle() { if(value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&)=delete;
};
using Held=std::unique_ptr<Handle>;
void stop(const std::atomic_bool& cancelled);
void guard(const QString& path,const QStringList& protectedFolders,bool generated=false);
std::vector<Held> parents(const QString& path);
Held open(const QString& path,bool shellDelete=false);
QJsonObject snapshot(HANDLE file,const std::atomic_bool& cancelled);
void expect(HANDLE file,const QJsonObject& expected,const std::atomic_bool& cancelled);
bool equal(HANDLE a,HANDLE b,const std::atomic_bool& cancelled);
bool sameFile(const QJsonObject& a,const QJsonObject& b);
QString rule(const QString& path,qint64 modified,bool installers,bool builds);
}
