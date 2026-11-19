#include "cleanup/cleanup_controller.h"
#include "cleanup/cleanup_service.h"
#include "organization/file_intelligence.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QtConcurrentRun>
using namespace filesearcher;
CleanupController::CleanupController(QString directory,QObject* parent):QObject(parent),directory_(std::move(directory)),database_(directory_+"/organization.sqlite"),protected_{directory_,QCoreApplication::applicationDirPath()} { pool_.setMaxThreadCount(1); QTimer::singleShot(0,this,&CleanupController::refresh); }
CleanupController::~CleanupController() { cancelled_=true; pool_.waitForDone(); }
void CleanupController::setAiEnabled(bool enabled) { ai_=enabled; related_.clear(); emit changed(); }
void CleanupController::cancel() { if(busy_) { cancelled_=true; message_="Cancelling after the current safe checkpoint…"; emit changed(); } }
void CleanupController::run(const QString& message,std::function<QVariantMap()> work) {
    if(busy_) return; busy_=true; cancelled_=false; message_=message; emit changed();
    auto* watcher=new QFutureWatcher<QVariantMap>(this);
    connect(watcher,&QFutureWatcher<QVariantMap>::finished,this,[this,watcher] {
        const auto r=watcher->result(); watcher->deleteLater(); busy_=false;
        if(r.contains("candidates")) candidates_=r["candidates"].toList();
        if(r.contains("related") && r["ai"].toBool()==ai_) related_=r["related"].toList();
        if(r.contains("history")) history_=r["history"].toList();
        if(r.contains("plan")) plan_=r["plan"].toMap();
        message_=r["message"].toString(); emit changed();
        if(r["review"].toBool()) emit reviewReady();
        if(r["filesChanged"].toBool()) emit filesChanged();
    });
    watcher->setFuture(QtConcurrent::run(&pool_,[this,work=std::move(work)] {
        try { return work(); } catch(const std::exception& e) {
            QVariantMap result{{"message",QString::fromUtf8(e.what())},{"plan",QVariantMap{}}};
            try { OrganizationStore store(database_); CleanupService service(store,protected_,cancelled_); service.recover(); result["history"]=service.history(); } catch(...) {}
            return result;
        }
    }));
}
void CleanupController::refresh() { run("Reading cleanup history…",[this] { OrganizationStore store(database_); CleanupService service(store,protected_,cancelled_); service.recover(); return QVariantMap{{"history",service.history()},{"message","History is retained when the search index is cleared. Restore recycled files through Windows Recycle Bin."}}; }); }
void CleanupController::scan(qint64 root,bool installers,bool builds) {
    if(busy_) return; candidates_.clear();
    run("Checking same-size files and opted-in rules. Hashing large files can take time…",[this,root,installers,builds] {
        OrganizationStore store(database_); CleanupService service(store,protected_,cancelled_); const auto found=service.scan(directory_+"/metadata.sqlite",root,installers,builds);
        return QVariantMap{{"candidates",found.candidates},{"message",QString("%1 suggestions; %2 files inspected; %3 unavailable, linked, protected or changed files skipped. %4Nothing recycled. Recycle Bin contents still use disk space.").arg(found.candidates.size()).arg(found.inspected).arg(found.skipped).arg(found.limited?"Limit reached (50,000 indexed files / 1,000 suggestions); choose a smaller indexed folder. ":"")}};
    });
}
void CleanupController::inspect(qint64 file) {
    if(busy_) return; related_.clear(); const bool ai=ai_;
    run("Looking for similar documents and possible versions…",[this,file,ai] {
        const auto source=organizationFile(directory_+"/metadata.sqlite",file);
        auto rows=relatedFiles(directory_+"/metadata.sqlite",directory_+"/vectors",file,cancelled_,ai);
        const auto base=[](const QString& path) { auto name=QFileInfo(path).completeBaseName().toCaseFolded(); name.remove(QRegularExpression("(?:[ _-]*(?:v(?:ersion)?[ _-]*)?\\d+|[ _-]*(?:final|copy|draft)|\\(\\d+\\))+$")); return name.trimmed(); };
        QVariantList result;
        for(auto value:rows) { auto row=value.toMap(); const bool version=!base(source["path"].toString()).isEmpty() && base(source["path"].toString())==base(row["path"].toString()) && QFileInfo(source["path"].toString()).suffix().compare(QFileInfo(row["path"].toString()).suffix(),Qt::CaseInsensitive)==0;
            row["kind"]=version?"Possible version":row["semanticScore"].toDouble()>=0.65?"Similar document":"Related filename/context";
            row["reason"]=row["reason"].toString()+". Source: "+source["name"].toString()+". Similarity and dates do not prove redundancy; no cleanup action is offered here.";
            row["modifiedLabel"]=QFileInfo(row["path"].toString()).lastModified().toUTC().toString(Qt::ISODate); result<<row;
        }
        return QVariantMap{{"related",result},{"ai",ai},{"message","These are discovery suggestions, not exact duplicate claims. Use the folder scan to verify exact copies."}};
    });
}
void CleanupController::prepare(const QString& token,bool reverse) {
    QVariantMap candidate; for(const auto& row:candidates_) if(row.toMap()["token"].toString()==token) candidate=row.toMap();
    run("Rechecking the proposed file and retained copy…",[this,candidate,reverse] { OrganizationStore store(database_); CleanupService service(store,protected_,cancelled_); const auto op=service.prepare(QJsonObject::fromVariantMap(candidate),reverse); return QVariantMap{{"plan",op.toVariantMap()},{"history",service.history()},{"review",true},{"message","Review the exact paths. Recycling requires separate approval."}}; });
}
void CleanupController::approve(const QString& id) {
    if(plan_["id"].toString()!=id) return;
    run("Revalidating and requesting Windows recycling…",[this,id] { OrganizationStore store(database_); CleanupService service(store,protected_,cancelled_); service.execute(id); return QVariantMap{{"plan",QVariantMap{}},{"candidates",QVariantList{}},{"history",service.history()},{"filesChanged",true},{"message","Windows confirmed recycling. Restore through Windows Recycle Bin; the app does not provide cleanup undo. Scan again for current suggestions."}}; });
}
void CleanupController::dismiss(const QString& id) { run("Cancelling cleanup review…",[this,id] { OrganizationStore store(database_); CleanupService service(store,protected_,cancelled_); service.cancel(id); return QVariantMap{{"plan",QVariantMap{}},{"history",service.history()},{"message","Review cancelled. Files unchanged."}}; }); }
