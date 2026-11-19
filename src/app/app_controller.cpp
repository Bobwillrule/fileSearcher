#include "app_controller.h"
#include "indexing/file_scanner.h"
#include "indexing/content_indexer.h"
#include "indexing/embedding_indexer.h"
#include "search/hybrid_search.h"
#include <QSettings>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QtConcurrentRun>
#include <algorithm>
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

namespace {
QStringList exclusionsFrom(const QString& value) {
    QStringList result;
    for (const auto& part : value.split(',')) if (!part.trimmed().isEmpty()) result << part.trimmed();
    return result;
}
struct SearchOutcome { filesearcher::SearchPage page; QString error; QString notice; };
}
AppController::AppController(const QString& dataDirectory, QObject* parent, IndexingTiming timing) : QObject(parent), dataDirectory_(dataDirectory),
    database_(QDir(dataDirectory).filePath("metadata.sqlite")), timing_(timing), cancelled_(std::make_shared<std::atomic_bool>(false)) {
    scanPool_.setMaxThreadCount(1); searchPool_.setMaxThreadCount(1);
    queryWorker_=new QObject;
    queryWorker_->moveToThread(&queryThread_);
    connect(&queryThread_,&QThread::finished,queryWorker_,&QObject::deleteLater);
    queryThread_.start();
    connect(&directoryWatcher_,&filesearcher::DirectoryWatcher::dirty,this,[this](qint64 root) { queueRoot(root,timing_.debounceMs); });
    connect(&directoryWatcher_,&filesearcher::DirectoryWatcher::coverageChanged,this,&AppController::updateMonitoring);
    connect(&scheduler_,&QTimer::timeout,this,&AppController::pollWork);
    connect(&periodic_,&QTimer::timeout,this,&AppController::reconcile);
    connect(&scanWatcher_, &QFutureWatcher<filesearcher::ScanReport>::finished, this, [this] {
        busy_ = false;
        const auto report = scanWatcher_.result();
        if (aiEnabled_ && !report.aiStatus.isEmpty()) aiStatus_=report.aiStatus;
        try {
            queue_->finish(activeWork_,report.errors == 0 && !report.cancelled,report.cancelled,QDateTime::currentMSecsSinceEpoch());
            pending_ = queue_->pending();
            status_ = paused_ ? "Indexing paused. Pending changes are saved." :
                (report.errors ? "Folder could not be fully updated. Previous records retained; retries are limited." :
                 QString("Index updated: %1 files, %2 text results processed, %3 content notices. Watching for changes.").arg(report.files).arg(report.contents).arg(report.contentWarnings));
            refreshRoots(); startSearch(); emit stateChanged(); pollWork();
        } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
    });
    try {
        if (!QDir().mkpath(dataDirectory)) throw std::runtime_error("Cannot create the local index directory");
        lock_ = std::make_unique<QLockFile>(QDir(dataDirectory).filePath("index.lock"));
        lock_->setStaleLockTime(0);
        if (!lock_->tryLock()) throw std::runtime_error("This index is already open in another FileSearcher window.");
        store_ = std::make_unique<filesearcher::MetadataStore>(database_, true);
        queue_ = std::make_unique<filesearcher::WorkQueue>(QDir(dataDirectory).filePath("work.sqlite"));
        paused_ = queue_->paused();
        QSettings settings(dataDirectory_+"/preferences.ini",QSettings::IniFormat);
        aiEnabled_=settings.value("ai/enabled",true).toBool();
        aiProfile_=std::clamp(settings.value("ai/profile",1).toInt(),0,2);
        if (!aiEnabled_) aiStatus_="AI indexing is off. Keyword search remains available.";
        for (const auto& root : store_->roots()) if (root.status == "Scanning") store_->rootStatus(root.id,"Interrupted; rescan needed");
        refreshRoots();
        QList<qint64> ids; for (const auto& root : indexedRoots_) ids << root.id;
        queue_->retain(ids);
        for (const auto& root : indexedRoots_) queueRoot(root.id,0);
        if (!roots_.isEmpty()) status_ = paused_ ? "Indexing paused. Resume to update saved results." : "Saved index loaded. Checking for changes…";
        organizer_=new OrganizationController(dataDirectory_,this);
        organizer_->setAiEnabled(aiEnabled_);
        connect(organizer_,&OrganizationController::filesChanged,this,[this] { loadPreview(0); scan(); });
        cleanup_=new CleanupController(dataDirectory_,this);
        cleanup_->setAiEnabled(aiEnabled_);
        connect(cleanup_,&CleanupController::filesChanged,this,[this] { loadPreview(0); scan(); });
        startSearch();
        scheduler_.start(timing_.tickMs); periodic_.start(timing_.reconcileMs); pollWork();
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); queue_.reset(); store_.reset(); }
}
AppController::~AppController() {
    delete cleanup_; cleanup_=nullptr;
    delete organizer_; organizer_=nullptr;
    scheduler_.stop(); periodic_.stop();
    disconnect(&directoryWatcher_,nullptr,this,nullptr);
    directoryWatcher_.synchronize({},{});
    cancelled_->store(true); searchCancelled_->store(true); scanPool_.waitForDone(); searchPool_.waitForDone();
    QMetaObject::invokeMethod(queryWorker_,[this] { querySession_.reset(); },Qt::BlockingQueuedConnection);
    queryThread_.quit(); queryThread_.wait();
}
void AppController::refreshRoots() {
    if (!store_) return;
    try {
        roots_.clear();
        indexedRoots_ = store_->roots();
        for (const auto& root : indexedRoots_) roots_ << QVariantMap{{"rootId",root.id},{"path",root.path},{"exclusions",root.exclusions.join(", ")},{"status",root.status}};
        directoryWatcher_.synchronize(indexedRoots_,{dataDirectory_});
        emit rootsChanged();
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
void AppController::addFolder(const QUrl& folder, const QString& exclusions) {
    if (busy_ || !store_) return;
    try {
        if (!folder.isLocalFile()) throw std::runtime_error("Choose a local folder");
        auto path = filesearcher::validatedRoot(folder.toLocalFile());
        if (filesearcher::withinPath(path, dataDirectory_)) throw std::runtime_error("The application's index folder cannot be indexed");
        const auto id = store_->addRoot(path, exclusionsFrom(exclusions));
        error_.clear(); refreshRoots(); queueRoot(id,0); pollWork();
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
void AppController::removeFolder(qint64 id) {
    if (busy_ || !store_) return;
    try {
        store_->removeRoot(id); error_.clear(); status_ = "Folder removed from index. Original files are unchanged.";
        queue_->remove(id); pending_ = queue_->pending();
        if (options_.root == id) options_.root = 0;
        refreshRoots(); startSearch(); emit stateChanged();
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
void AppController::changeExclusions(qint64 id, const QString& exclusions) {
    if (busy_ || !store_) return;
    try { store_->updateExclusions(id, exclusionsFrom(exclusions)); error_.clear(); refreshRoots(); queueRoot(id,0); pollWork(); }
    catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
void AppController::scan() {
    if (!store_ || !queue_) return;
    error_.clear();
    for (const auto& root : indexedRoots_) queueRoot(root.id,0);
    status_ = paused_ ? "Rescan saved. Resume indexing to run it." : "Checking selected folders…";
    emit stateChanged(); pollWork();
}
void AppController::queueRoot(qint64 id, int delayMs) {
    if (!queue_) return;
    try {
        for (const auto& root : indexedRoots_) if (root.id == id) {
            queue_->enqueue(id,root.path,QDateTime::currentMSecsSinceEpoch(),delayMs);
            // Fresh filesystem work takes priority over the current AI pass.
            if (busy_ && id==activeWork_.root) aiCancelled_->store(true);
            pending_ = queue_->pending(); emit stateChanged(); break;
        }
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
void AppController::updateMonitoring() {
    monitoring_ = directoryWatcher_.unavailable().isEmpty() ? "Automatic updates enabled" :
        QString("%1 folder(s) awaiting live monitoring; periodic checks remain enabled").arg(directoryWatcher_.unavailable().size());
    emit stateChanged();
}
void AppController::reconcile() {
    if (!store_ || !queue_) return;
    directoryWatcher_.synchronize(indexedRoots_,{dataDirectory_});
    for (const auto& root : indexedRoots_) queueRoot(root.id,0);
    pollWork();
}
void AppController::pollWork() {
    if (busy_ || paused_ || !queue_ || !store_) return;
    filesearcher::IndexedRoot selected;
    try {
        auto work = queue_->take(QDateTime::currentMSecsSinceEpoch()); if (!work) return;
        activeWork_ = *work;
        const auto root = std::find_if(indexedRoots_.begin(),indexedRoots_.end(),[&](const auto& value) { return value.id == work->root && value.path == work->path; });
        if (root == indexedRoots_.end()) { queue_->remove(work->root); return; }
        selected = *root;
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); return; }
    busy_ = true; cancelled_->store(false); error_.clear(); status_ = "Scanning metadata…"; emit stateChanged();
    const auto database = database_, protectedDirectory = dataDirectory_;
    const auto cancelled = cancelled_;
    const auto worker = QCoreApplication::applicationDirPath()+"/document_worker.exe";
    const bool contentEnabled = timing_.contentEnabled;
    const bool embeddingsEnabled=contentEnabled && timing_.embeddingsEnabled && aiEnabled_;
    const int profile=aiProfile_; const auto aiCancelled=aiCancelled_; aiCancelled->store(false);
    const auto appDirectory=QCoreApplication::applicationDirPath();
    scanWatcher_.setFuture(QtConcurrent::run(&scanPool_, [this,database,protectedDirectory,cancelled,selected,worker,contentEnabled,embeddingsEnabled,profile,aiCancelled,appDirectory] {
        try {
            filesearcher::MetadataStore store(database);
            auto report = filesearcher::scanRoot(store, selected, {protectedDirectory}, *cancelled, [this](qint64 count) {
                QMetaObject::invokeMethod(this, [this,count] { if (!paused_) status_ = QString("Checking changes: %1 file entries examined").arg(count); emit stateChanged(); }, Qt::QueuedConnection);
            });
            if (contentEnabled && !report.cancelled && report.errors == 0) {
                const auto content = filesearcher::indexContents(store,selected.id,worker,*cancelled,[this](int count) {
                    QMetaObject::invokeMethod(this,[this,count] { if (!paused_) status_ = QString("Reading file contents: %1 files processed").arg(count); emit stateChanged(); },Qt::QueuedConnection);
                });
                report.cancelled = content.cancelled; report.errors += content.retries;
                report.contents = content.processed; report.contentWarnings = content.warnings;
            }
            // Publish lexical results before AI work, which can take longer on a large folder.
            QMetaObject::invokeMethod(this,[this] { startSearch(); },Qt::QueuedConnection);
            if (embeddingsEnabled && !report.cancelled && !aiCancelled->load()) {
                QMetaObject::invokeMethod(this,[this] { if (aiEnabled_) aiStatus_="Preparing local AI index…"; emit stateChanged(); },Qt::QueuedConnection);
                const auto ai=filesearcher::indexEmbeddings(database,selected.id,appDirectory+"/embedding_worker.exe",
                    appDirectory+"/models/bge-small-en-v1.5",protectedDirectory+"/vectors",profile,*cancelled,*aiCancelled,[this](int count) {
                        QMetaObject::invokeMethod(this,[this,count] { if (aiEnabled_) aiStatus_=QString("Saving local AI embeddings: %1 new chunks").arg(count); emit stateChanged(); },Qt::QueuedConnection);
                    });
                report.aiStatus=ai.status; report.cancelled=cancelled->load();
            }
            return report;
        } catch (const std::exception& error) { filesearcher::ScanReport report; report.errors = 1; report.message = QString::fromUtf8(error.what()); return report; }
    }));
}
void AppController::cancel() { setPaused(true); }
void AppController::configureAi(bool enabled,int profile) {
    profile=std::clamp(profile,0,2);
    if (enabled==aiEnabled_ && profile==aiProfile_) return;
    QSettings settings(dataDirectory_+"/preferences.ini",QSettings::IniFormat);
    settings.setValue("ai/enabled",enabled); settings.setValue("ai/profile",profile); settings.sync();
    if (settings.status()!=QSettings::NoError) { error_="Cannot save AI preferences."; emit stateChanged(); return; }
    aiEnabled_=enabled; aiProfile_=profile; aiCancelled_->store(true);
    if(organizer_) organizer_->setAiEnabled(enabled);
    if(cleanup_) cleanup_->setAiEnabled(enabled);
    if (options_.mode==5) startSearch();
    aiStatus_=enabled ? "AI settings saved. Pending chunks will resume." : "AI indexing is off. Keyword search remains available.";
    if (enabled) for (const auto& root:indexedRoots_) queueRoot(root.id,0);
    emit stateChanged(); pollWork();
}
void AppController::setPaused(bool paused) {
    if (!queue_) return;
    try {
        queue_->setPaused(paused); paused_ = paused;
        if (paused_) cancelled_->store(true);
        status_ = paused_ ? "Indexing paused. Pending changes are saved." : "Indexing resumed. Checking pending changes…";
        emit stateChanged(); if (!paused_) pollWork();
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
void AppController::search(const QString& query, const QString& extension, qint64 root, const QString& minimumBytes, const QString& maximumBytes, int days, int mode) {
    bool minValid = true, maxValid = true;
    const auto min = minimumBytes.trimmed().isEmpty() ? -1 : minimumBytes.toLongLong(&minValid);
    const auto max = maximumBytes.trimmed().isEmpty() ? -1 : maximumBytes.toLongLong(&maxValid);
    if (!minValid || !maxValid || (!minimumBytes.trimmed().isEmpty() && min < 0) || (!maximumBytes.trimmed().isEmpty() && max < 0) || (max >= 0 && min > max)) {
        filtersValid_ = false;
        searchCancelled_->store(true); searchNotice_.clear();
        ++searchGeneration_; searching_ = false; results_.clear(); total_ = 0;
        error_ = "Size filters must be nonnegative bytes, with minimum no greater than maximum.";
        emit resultsChanged(); emit stateChanged(); return;
    }
    options_ = {query,extension,root,min,max,days > 0 ? QDateTime::currentDateTimeUtc().addDays(-days).toMSecsSinceEpoch() : -1,500};
    options_.mode = std::clamp(mode,0,5);
    filtersValid_ = true; error_.clear(); startSearch();
}
void AppController::reindexContent() {
    if (busy_ || !store_) return;
    try { store_->resetContent(); loadPreview(0); startSearch(); scan(); }
    catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
void AppController::refreshStorage() {
    if (!store_) return;
    const auto database=database_,data=dataDirectory_,model=QCoreApplication::applicationDirPath()+"/models/bge-small-en-v1.5";
    auto* watcher=new QFutureWatcher<QVariantMap>(this);
    connect(watcher,&QFutureWatcher<QVariantMap>::finished,this,[this,watcher] { storageInfo_=watcher->result(); watcher->deleteLater(); emit storageChanged(); });
    watcher->setFuture(QtConcurrent::run(&searchPool_,[database,data,model] {
        QVariantMap result{{"indexPath",QFileInfo(data).absoluteFilePath()},{"modelPath",model}};
        try { filesearcher::MetadataStore store(database); auto stats=store.statistics(); for (auto i=stats.begin();i!=stats.end();++i) result[i.key()]=i.value(); }
        catch (...) { result["error"]="Storage counts are temporarily unavailable."; }
        qint64 indexBytes=0,modelBytes=0;
        for (const auto& name:{"metadata.sqlite","metadata.sqlite-wal","metadata.sqlite-shm","work.sqlite","work.sqlite-wal","work.sqlite-shm"}) indexBytes+=QFileInfo(data+'/'+name).size();
        const QDir vectors(data+"/vectors");
        for (const auto& file:vectors.entryInfoList({"graph-*.hnsw","graph-*.hnsw.part","current.json"},QDir::Files|QDir::NoSymLinks)) indexBytes+=file.size();
        for (const auto& name:{"onnx/model.onnx","vocab.txt","README.md","LICENSE","model-assets.lock.json"}) modelBytes+=QFileInfo(model+'/'+name).size();
        result["indexBytes"]=indexBytes; result["modelBytes"]=modelBytes;
        qint64 organizationBytes=0;
        for(const auto& name:{"organization.sqlite","organization.sqlite-wal","organization.sqlite-shm"}) organizationBytes+=QFileInfo(data+'/'+name).size();
        result["organizationBytes"]=organizationBytes;
        return result;
    }));
}
void AppController::releaseAiMemory() {
    searchCancelled_->store(true); ++searchGeneration_; searching_=false;
    QMetaObject::invokeMethod(queryWorker_,[this] { if (querySession_) querySession_->release(); },Qt::QueuedConnection);
    searchNotice_="AI memory will be released. The next AI search reloads the model."; emit stateChanged();
}
void AppController::openStorageFolder(bool model) {
    const auto path=model?QCoreApplication::applicationDirPath()+"/models/bge-small-en-v1.5":dataDirectory_;
    if (!QFileInfo(path).isDir()) { error_="This storage folder is unavailable."; emit stateChanged(); return; }
    const auto native=QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",native.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32) {
        error_="Windows could not open the storage folder."; emit stateChanged();
    }
}
void AppController::clearSavedIndex(bool embeddingsOnly) {
    if(organizer_ && organizer_->busy()) { error_="Wait for the organization operation to finish before clearing the index."; emit stateChanged(); return; }
    if(cleanup_ && cleanup_->busy()) { error_="Wait for the cleanup operation to finish before clearing the index."; emit stateChanged(); return; }
    if (busy_ || !store_ || !queue_) return;
    searchCancelled_->store(true); ++searchGeneration_; searching_=false;
    ++previewGeneration_; preview_.clear(); previewLoading_=false; emit previewChanged();
    busy_=true; maintaining_=true; status_=embeddingsOnly?"Rebuilding saved AI data…":"Clearing saved search data…"; emit stateChanged();
    const auto database=database_,data=dataDirectory_;
    // Drain query resources on their owning thread, then perform maintenance away from the UI.
    QMetaObject::invokeMethod(queryWorker_,[this,database,data,embeddingsOnly] {
        if (querySession_) querySession_->release();
        QMetaObject::invokeMethod(this,[this,database,data,embeddingsOnly] {
            auto* watcher=new QFutureWatcher<QString>(this);
            connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,embeddingsOnly] {
                const auto message=watcher->result(); watcher->deleteLater(); busy_=false; maintaining_=false;
                try {
                    QList<qint64> ids; for (const auto& root:store_->roots()) ids<<root.id;
                    queue_->retain(ids); pending_=queue_->pending();
                    if (!embeddingsOnly) options_.root=0;
                    refreshRoots(); startSearch(); refreshStorage();
                    error_=message;
                    status_=message.isEmpty()?(embeddingsOnly?"AI data cleared; indexing will rebuild it.":"Search data cleared. Original files are unchanged. Add a folder to begin again."):"Storage maintenance needs attention. Original files are unchanged.";
                    aiStatus_="Local AI waits for indexed text.";
                    if (embeddingsOnly) scan();
                } catch (const std::exception& e) { error_=QString::fromUtf8(e.what()); }
                emit stateChanged();
            });
            watcher->setFuture(QtConcurrent::run(&scanPool_,[database,data,embeddingsOnly] {
                try {
                    filesearcher::MetadataStore store(database); store.clearSavedIndex(embeddingsOnly);
                    const auto cachePath=data+"/vectors"; QFileInfo folder(cachePath);
                    // Never recurse or traverse a substituted cache directory/link.
                    if (folder.isSymLink() || folder.isJunction()) return QString("Database cleared; linked cache folder was left untouched.");
                    QDir cache(cachePath); static const QRegularExpression owned("^graph-[0-9a-f-]{36}\\.hnsw(?:\\.part)?$");
                    for (const auto& file:cache.entryInfoList(QDir::Files|QDir::NoSymLinks))
                        if (file.fileName()=="current.json" || owned.match(file.fileName()).hasMatch())
                            if (!cache.remove(file.fileName())) return QString("Database cleared; some cache files could not be removed.");
                    return QString();
                } catch (const std::exception& e) { return QString::fromUtf8(e.what()); }
            }));
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
void AppController::loadPreview(qint64 file, qint64 passage, int ordinal) {
    const auto generation = ++previewGeneration_;
    preview_.clear(); previewLoading_ = file > 0 && store_ != nullptr; emit previewChanged();
    if (!previewLoading_) return;
    const auto database = database_;
    auto* watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher,&QFutureWatcher<QVariantMap>::finished,this,[this,watcher,generation] {
        const auto result = watcher->result(); watcher->deleteLater();
        if (generation != previewGeneration_) return;
        preview_ = result; previewLoading_ = false; emit previewChanged();
    });
    watcher->setFuture(QtConcurrent::run(&searchPool_,[database,file,passage,ordinal] {
        try { filesearcher::MetadataStore store(database); return store.preview(file,passage,ordinal); }
        catch (...) { return QVariantMap{{"status","preview_unavailable"},{"text","Preview is unavailable."}}; }
    }));
}
void AppController::startSearch() {
    if (maintaining_) return;
    if (!store_) return;
    if (!filtersValid_) {
        error_ = "Size filters must be nonnegative bytes, with minimum no greater than maximum.";
        emit stateChanged(); return;
    }
    const auto generation = ++searchGeneration_;
    searchCancelled_->store(true);
    searchCancelled_=std::make_shared<std::atomic_bool>(false);
    const auto cancellation=searchCancelled_;
    const auto database = database_;
    const auto options = options_;
    const auto cache=dataDirectory_+"/vectors",appDirectory=QCoreApplication::applicationDirPath();
    const bool aiEnabled=aiEnabled_;
    searchNotice_.clear();
    searching_ = true; emit stateChanged();
    QMetaObject::invokeMethod(queryWorker_,[this,database,options,cancellation,cache,appDirectory,aiEnabled,generation] {
        SearchOutcome outcome;
        if (cancellation->load()) return;
        try {
            if (!querySession_) querySession_=std::make_unique<filesearcher::SearchSession>(database,cache,appDirectory+"/embedding_worker.exe",appDirectory+"/models/bge-small-en-v1.5");
            auto result=querySession_->search(options,aiEnabled,cancellation);
            outcome={std::move(result.page),{},result.notice};
        } catch (const std::exception& error) { outcome.error=QString::fromUtf8(error.what()); }
        QMetaObject::invokeMethod(this,[this,generation,outcome=std::move(outcome)] {
            if (generation!=searchGeneration_) return;
            searching_=false; error_=outcome.error; searchNotice_=outcome.notice;
            results_=outcome.page.rows; total_=outcome.page.total;
            emit resultsChanged(); emit stateChanged();
        },Qt::QueuedConnection);
    },Qt::QueuedConnection);
}
void AppController::openFile(qint64 id, bool reveal) {
    if (!store_) return;
    try {
        const auto path = store_->filePath(id);
        if (path.isEmpty() || !QFileInfo(path).isFile()) throw std::runtime_error("This file is no longer available. Rescan its folder.");
        const auto native = QDir::toNativeSeparators(path).toStdWString();
        if (reveal) {
            const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            PIDLIST_ABSOLUTE item = nullptr;
            HRESULT result = SHParseDisplayName(native.c_str(), nullptr, &item, 0, nullptr);
            if (SUCCEEDED(result)) result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            CoTaskMemFree(item);
            if (SUCCEEDED(init)) CoUninitialize();
            if (FAILED(result)) throw std::runtime_error("Explorer could not reveal this file");
        } else {
            SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_FLAG_NO_UI;
            info.lpVerb = L"open"; info.lpFile = native.c_str(); info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info)) throw std::runtime_error("Windows could not open this file in its default application");
        }
        error_.clear(); emit stateChanged();
    } catch (const std::exception& error) { error_ = QString::fromUtf8(error.what()); emit stateChanged(); }
}
