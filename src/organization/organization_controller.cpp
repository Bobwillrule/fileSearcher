#include "organization/organization_controller.h"
#include "organization/file_intelligence.h"
#include "organization/move_service.h"
#include "storage/metadata_store.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrentRun>
using namespace filesearcher;
OrganizationController::OrganizationController(QString directory,QObject* parent):QObject(parent),directory_(std::move(directory)),database_(directory_+"/organization.sqlite"),metadata_(directory_+"/metadata.sqlite"),protected_{directory_,QCoreApplication::applicationDirPath()} {
    pool_.setMaxThreadCount(1); QTimer::singleShot(0,this,&OrganizationController::refresh);
}
OrganizationController::~OrganizationController() { cancelled_->store(true); pool_.waitForDone(); }
void OrganizationController::setAiEnabled(bool enabled) {
    if(useSemantics_==enabled) return;
    useSemantics_=enabled; related_.clear(); suggestions_.clear(); message_="AI preferences changed. Inspect again to refresh the evidence."; emit changed();
}
void OrganizationController::run(const QString& status,std::function<QVariantMap()> work) {
    if(busy_) return; busy_=true; message_=status; emit changed();
    auto* watcher=new QFutureWatcher<QVariantMap>(this);
    connect(watcher,&QFutureWatcher<QVariantMap>::finished,this,[this,watcher] {
        const auto result=watcher->result(); watcher->deleteLater(); busy_=false;
        if(result.contains("useSemantics") && result["useSemantics"].toBool()!=useSemantics_) { emit changed(); return; }
        if(result.contains("categories")) categories_=result["categories"].toList();
        if(result.contains("related")) related_=result["related"].toList();
        if(result.contains("suggestions")) suggestions_=result["suggestions"].toList();
        if(result.contains("history")) history_=result["history"].toList();
        if(result.contains("members")) members_=result["members"].toList();
        if(result.contains("selected")) selected_=result["selected"].toMap();
        if(result.contains("movePlan")) movePlan_=result["movePlan"].toMap();
        message_=result.value("message","Ready").toString(); emit changed();
        if(result.value("review").toBool()) emit movePlanReady();
        if(result.value("filesChanged").toBool()) emit filesChanged();
    });
    const auto database=database_; const auto folders=protected_; const auto cancellation=cancelled_;
    watcher->setFuture(QtConcurrent::run(&pool_,[work=std::move(work),database,folders,cancellation] {
        try { return work(); }
        catch(const std::exception& e) {
            QVariantMap result{{"message",QString::fromUtf8(e.what())}};
            try { OrganizationStore store(database); MoveService(store,folders,cancellation.get()).recover(); result["history"]=store.history(); } catch(...) {}
            return result;
        }
    }));
}
void OrganizationController::refresh() {
    run("Checking saved collections and interrupted moves…",[this] {
        OrganizationStore store(database_); MoveService(store,protected_,cancelled_.get()).recover();
        return QVariantMap{{"categories",store.categories()},{"history",store.history()},{"message","Collections do not move files. File moves always require a separate review."}};
    });
}
void OrganizationController::inspect(qint64 id) {
    if(busy_) return; related_.clear(); suggestions_.clear(); selected_.clear();
    const auto useSemantics=useSemantics_;
    run("Finding indexed evidence and category matches…",[this,id,useSemantics] {
        OrganizationStore store(database_); const auto source=organizationFile(metadata_,id);
        const auto related=relatedFiles(metadata_,directory_+"/vectors",id,*cancelled_,useSemantics),suggestions=categorySuggestions(metadata_,store,id,*cancelled_,useSemantics);
        return QVariantMap{{"useSemantics",useSemantics},{"selected",source},{"related",related},{"suggestions",suggestions},{"categories",store.categories()},
            {"message",suggestions.isEmpty()?"No unambiguous category suggestion. You can choose a virtual collection yourself. Relatedness is a ranking signal, not proof.":"Review the indexed evidence. Accept adds a virtual collection tag; moving is a separate action."}};
    });
}
void OrganizationController::saveCategory(qint64 id,const QString& name,const QString& description,const QString& destination) {
    run("Saving category…",[this,id,name,description,destination] { OrganizationStore store(database_); store.saveCategory(id,name,description,destination); return QVariantMap{{"categories",store.categories()},{"suggestions",QVariantList{}},{"message","Category saved. Inspect a file to update suggestions."}}; });
}
void OrganizationController::removeCategory(qint64 id) {
    run("Removing virtual category…",[this,id] { OrganizationStore store(database_); store.removeCategory(id); return QVariantMap{{"categories",store.categories()},{"suggestions",QVariantList{}},{"members",QVariantList{}},{"message","Category and tags removed. Original files are unchanged."}}; });
}
void OrganizationController::addExample(qint64 category) {
    const auto id=selected_.value("fileId").toLongLong();
    run("Saving category example…",[this,id,category] { const auto file=organizationFile(metadata_,id); OrganizationStore store(database_); store.addExample(category,file["path"].toString()); return QVariantMap{{"categories",store.categories()},{"suggestions",QVariantList{}},{"message","Example saved. Its indexed passages inform future suggestions; inspect again to refresh."}}; });
}
void OrganizationController::clearExamples(qint64 category) {
    run("Clearing category examples…",[this,category] { OrganizationStore store(database_); store.query("UPDATE categories SET examples='[]',revision=revision+1 WHERE id=?",{category}); return QVariantMap{{"categories",store.categories()},{"suggestions",QVariantList{}},{"message","Examples cleared. Original files are unchanged."}}; });
}
void OrganizationController::decide(qint64 category,bool accept) {
    const auto id=selected_.value("fileId").toLongLong();
    run("Saving review decision…",[this,id,category,accept] { const auto file=organizationFile(metadata_,id); OrganizationStore store(database_); store.decide(category,file["path"].toString(),file["size"].toLongLong(),file["modified"].toLongLong(),accept?"accepted":"rejected"); return QVariantMap{{"categories",store.categories()},{"suggestions",QVariantList{}},{"message",accept?"Added to the virtual collection. The file has not moved.":"Suggestion dismissed and tag removed. The file has not moved."}}; });
}
void OrganizationController::showMembers(qint64 category) {
    run("Loading virtual collection…",[this,category] { OrganizationStore store(database_); return QVariantMap{{"members",store.members(category)},{"message","Collection membership is saved by path. Missing paths remain visible until you remove their tags."}}; });
}
void OrganizationController::removeMember(qint64 category,const QString& path) {
    run("Removing a virtual tag…",[this,category,path] { OrganizationStore store(database_); store.query("DELETE FROM members WHERE category=? AND path_key=?",{category,pathKey(path)}); store.query("DELETE FROM decisions WHERE category=? AND path=? AND status='accepted'",{category,pathKey(path)}); return QVariantMap{{"members",store.members(category)},{"categories",store.categories()},{"message","Tag removed. Original files are unchanged."}}; });
}
void OrganizationController::prepareMove(const QString& destination) {
    const auto id=selected_.value("fileId").toLongLong();
    run("Verifying the source and preparing a move for review…",[this,id,destination] { const auto file=organizationFile(metadata_,id); OrganizationStore store(database_); auto plan=MoveService(store,protected_,cancelled_.get()).prepare(file["path"].toString(),destination); return QVariantMap{{"movePlan",plan.toVariantMap()},{"review",true},{"history",store.history()},{"message","Review the exact source and destination before approving."}}; });
}
void OrganizationController::prepareUndo(const QString& id) {
    run("Checking whether this move can be undone…",[this,id] { OrganizationStore store(database_); auto plan=MoveService(store,protected_,cancelled_.get()).prepareUndo(id); return QVariantMap{{"movePlan",plan.toVariantMap()},{"review",true},{"history",store.history()}}; });
}
void OrganizationController::reviewMove(const QString& id) {
    run("Loading the move for review…",[this,id] { OrganizationStore store(database_); auto plan=store.operation(id); if(plan["state"]!="planned" && plan["state"]!="copy_ready") throw std::runtime_error("This operation requires manual review"); return QVariantMap{{"movePlan",plan.toVariantMap()},{"review",true}}; });
}
void OrganizationController::approveMove(const QString& id) {
    if(movePlan_.value("id").toString()!=id) return;
    run("Applying the approved move; file checks remain active…",[this,id] { OrganizationStore store(database_); MoveService(store,protected_,cancelled_.get()).execute(id); return QVariantMap{{"movePlan",QVariantMap{}},{"selected",QVariantMap{}},{"related",QVariantList{}},{"suggestions",QVariantList{}},{"history",store.history()},{"categories",store.categories()},{"filesChanged",true},{"message","Move completed. Indexed folders will refresh when indexing is active. The history offers undo."}}; });
}
void OrganizationController::cancelMove(const QString& id) {
    run("Cancelling the unstarted plan…",[this,id] { OrganizationStore store(database_); MoveService(store,protected_,cancelled_.get()).cancel(id); return QVariantMap{{"movePlan",QVariantMap{}},{"history",store.history()},{"message","Move cancelled. Original files are unchanged."}}; });
}
