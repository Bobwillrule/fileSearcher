#pragma once
#include "storage/metadata_store.h"
#include "indexing/file_scanner.h"
#include "indexing/work_queue.h"
#include "indexing/directory_watcher.h"
#include <QObject>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QUrl>
#include <QTimer>
#include <QLockFile>
#include <QThread>
#include "search/hybrid_search.h"
#include <memory>
#include "organization/organization_controller.h"

struct IndexingTiming { int debounceMs = 500; int tickMs = 100; int reconcileMs = 300000; bool contentEnabled = true; bool embeddingsEnabled = true; };
class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList roots READ roots NOTIFY rootsChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(qint64 total READ total NOTIFY resultsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(int pending READ pending NOTIFY stateChanged)
    Q_PROPERTY(QString monitoring READ monitoring NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(bool aiEnabled READ aiEnabled NOTIFY stateChanged)
    Q_PROPERTY(int aiProfile READ aiProfile NOTIFY stateChanged)
    Q_PROPERTY(QString aiStatus READ aiStatus NOTIFY stateChanged)
    Q_PROPERTY(QString searchNotice READ searchNotice NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap storageInfo READ storageInfo NOTIFY storageChanged)
    Q_PROPERTY(QObject* organizer READ organizer CONSTANT)
public:
    explicit AppController(const QString& dataDirectory, QObject* parent = nullptr, IndexingTiming timing = {});
    ~AppController() override;
    QVariantList roots() const { return roots_; }
    QVariantList results() const { return results_; }
    qint64 total() const { return total_; }
    bool busy() const { return busy_; }
    bool searching() const { return searching_; }
    QString status() const { return status_; }
    QString error() const { return error_; }
    bool paused() const { return paused_; }
    int pending() const { return pending_; }
    QString monitoring() const { return monitoring_; }
    QVariantMap preview() const { return preview_; }
    bool previewLoading() const { return previewLoading_; }
    bool aiEnabled() const { return aiEnabled_; }
    int aiProfile() const { return aiProfile_; }
    QString aiStatus() const { return aiStatus_; }
    QString searchNotice() const { return searchNotice_; }
    QVariantMap storageInfo() const { return storageInfo_; }
    QObject* organizer() const { return organizer_; }
    Q_INVOKABLE void refreshStorage();
    Q_INVOKABLE void clearSavedIndex(bool embeddingsOnly=false);
    Q_INVOKABLE void releaseAiMemory();
    Q_INVOKABLE void openStorageFolder(bool model);
    Q_INVOKABLE void configureAi(bool enabled,int profile);
    Q_INVOKABLE void addFolder(const QUrl& folder, const QString& exclusions);
    Q_INVOKABLE void removeFolder(qint64 id);
    Q_INVOKABLE void changeExclusions(qint64 id, const QString& exclusions);
    Q_INVOKABLE void scan();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void search(const QString& query, const QString& extension, qint64 root, const QString& minimumBytes, const QString& maximumBytes, int days, int mode = 0);
    Q_INVOKABLE void loadPreview(qint64 file, qint64 passage = 0, int ordinal = -1);
    Q_INVOKABLE void reindexContent();
    Q_INVOKABLE void openFile(qint64 id, bool reveal);
signals:
    void rootsChanged();
    void resultsChanged();
    void stateChanged();
    void previewChanged();
    void storageChanged();
private:
    OrganizationController* organizer_=nullptr;
    void refreshRoots();
    void startSearch();
    void queueRoot(qint64 root, int delayMs);
    void pollWork();
    void reconcile();
    void updateMonitoring();
    QString dataDirectory_, database_;
    std::unique_ptr<filesearcher::MetadataStore> store_;
    std::unique_ptr<QLockFile> lock_;
    std::unique_ptr<filesearcher::WorkQueue> queue_;
    filesearcher::DirectoryWatcher directoryWatcher_;
    QList<filesearcher::IndexedRoot> indexedRoots_;
    filesearcher::IndexWork activeWork_;
    IndexingTiming timing_;
    QTimer scheduler_, periodic_;
    QVariantList roots_, results_;
    qint64 total_ = 0;
    bool busy_ = false, searching_ = false, filtersValid_ = true;
    bool paused_ = false;
    bool maintaining_ = false;
    int pending_ = 0;
    QString monitoring_;
    QString status_ = "Add a folder to begin", error_;
    filesearcher::SearchOptions options_;
    quint64 searchGeneration_ = 0;
    quint64 previewGeneration_ = 0;
    QVariantMap preview_;
    bool previewLoading_ = false;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::shared_ptr<std::atomic_bool> searchCancelled_ = std::make_shared<std::atomic_bool>(false);
    QString searchNotice_;
    QVariantMap storageInfo_;
    std::shared_ptr<std::atomic_bool> aiCancelled_ = std::make_shared<std::atomic_bool>(false);
    bool aiEnabled_ = true;
    int aiProfile_ = 1;
    QString aiStatus_ = "Local AI waits for indexed text.";
    QFutureWatcher<filesearcher::ScanReport> scanWatcher_;
    QThreadPool scanPool_, searchPool_;
    QThread queryThread_;
    QObject* queryWorker_ = nullptr;
    std::unique_ptr<filesearcher::SearchSession> querySession_;
};
