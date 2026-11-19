#include "cleanup/recycle_file.h"
#include <QDir>
#include <QFileInfo>
#include <stdexcept>
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace filesearcher {
bool recyclingAllowed(unsigned long flags) { return (flags&TSF_DELETE_RECYCLE_IF_POSSIBLE)!=0; }
namespace {
void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error(QString("Windows could not recycle this file (0x%1). No permanent-delete fallback is used.").arg(quint32(hr),8,16,QChar('0')).toStdString()); }
QString itemPath(IShellItem* item) { PWSTR p=nullptr; if(!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&p))) return {}; auto result=QDir::fromNativeSeparators(QString::fromWCharArray(p)); CoTaskMemFree(p); return result; }
class Sink final:public IFileOperationProgressSink {
public:
    std::function<void()> validate;
    const std::atomic_bool& cancelled;
    QString failure,recycledPath;
    HRESULT outcome=E_PENDING;
    explicit Sink(std::function<void()> v,const std::atomic_bool& c):validate(std::move(v)),cancelled(c) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override { if(!out) return E_POINTER; *out=nullptr; if(iid==IID_IUnknown || iid==IID_IFileOperationProgressSink) { *out=static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK; } return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n=--references; if(!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return cancelled?E_ABORT:S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags,IShellItem*) override {
        if(cancelled || !recyclingAllowed(flags)) { failure="Recycling is unavailable or cancelled. Permanent deletion was refused."; return E_ABORT; }
        try { validate(); return S_OK; } catch(const std::exception& e) { failure=QString::fromUtf8(e.what()); return E_ABORT; }
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD,IShellItem*,HRESULT hr,IShellItem* created) override { outcome=hr; recycledPath=itemPath(created); return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD,IShellItem*,LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD,IShellItem*,LPCWSTR,HRESULT,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD,IShellItem*,IShellItem*,LPCWSTR,HRESULT,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD,IShellItem*,LPCWSTR) override { return E_ABORT; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD,IShellItem*,LPCWSTR,LPCWSTR,DWORD,HRESULT,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT,UINT) override { return cancelled?E_ABORT:S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
private:
    std::atomic_ulong references=1;
};
}
QString recycleFile(const QString& path,const std::function<void()>& beforeDelete,const std::atomic_bool& cancelled) {
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE); check(initialized);
    struct Apartment { ~Apartment(){CoUninitialize();} } apartment;
    ComPtr<IFileOperation> operation; check(CoCreateInstance(CLSID_FileOperation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&operation)));
    check(operation->SetOperationFlags(FOFX_RECYCLEONDELETE|FOFX_EARLYFAILURE|FOF_NOERRORUI|FOF_SILENT|FOF_NOCONFIRMATION|FOF_NO_CONNECTED_ELEMENTS|FOF_NORECURSION));
    ComPtr<IShellItem> item; check(SHCreateItemFromParsingName(QDir::toNativeSeparators(path).toStdWString().c_str(),nullptr,IID_PPV_ARGS(&item)));
    ComPtr<Sink> sink; sink.Attach(new Sink(beforeDelete,cancelled));
    check(operation->DeleteItem(item.Get(),sink.Get())); const auto result=operation->PerformOperations(); BOOL aborted=FALSE; check(operation->GetAnyOperationsAborted(&aborted));
    if(!sink->failure.isEmpty()) throw std::runtime_error(sink->failure.toStdString());
    check(result); check(sink->outcome);
    if(aborted || QFileInfo::exists(path) || sink->recycledPath.isEmpty()) throw std::runtime_error("Windows did not confirm a Recycle Bin destination. Review this entry in history and Windows Recycle Bin; it will not be retried automatically.");
    return sink->recycledPath;
}
}
