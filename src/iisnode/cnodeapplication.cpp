#include "precomp.h"

CNodeApplication::CNodeApplication(CNodeApplicationManager* applicationManager, BOOL isDebugger, NodeDebugCommand debugCommand, DWORD debugPort)
    : applicationManager(applicationManager), scriptName(NULL), processManager(NULL),
    isDebugger(isDebugger), peerApplication(NULL), debugCommand(debugCommand), 
    debugPort(debugPort), needsRecycling(FALSE), configPath(NULL)
{
}

CNodeApplication::~CNodeApplication()
{
    this->Cleanup();
}

void CNodeApplication::Cleanup()
{
    this->GetApplicationManager()->GetFileWatcher()->RemoveWatch(this);

    if (NULL != this->scriptName)
    {
        delete [] this->scriptName;
        this->scriptName = NULL;
    }

    if (NULL != this->configPath)
    {
        delete [] this->configPath;
        this->configPath = NULL;
    }

    if (NULL != this->processManager)
    {
        this->processManager->DecRef(); // incremented in CNodeProcessManager::ctor
        this->processManager = NULL;
    }
}

HRESULT CNodeApplication::Initialize(PCWSTR scriptName, IHttpContext* context)
{
    HRESULT hr;

    CheckNull(scriptName);

    DWORD len = wcslen(scriptName) + 1;
    ErrorIf(NULL == (this->scriptName = new WCHAR[len]), ERROR_NOT_ENOUGH_MEMORY);
    wcscpy(this->scriptName, scriptName);

    ErrorIf(NULL == (this->processManager = new	CNodeProcessManager(this, context)), ERROR_NOT_ENOUGH_MEMORY);
    CheckError(this->processManager->Initialize(context));
    
    CheckError(this->SetConfigPath(context));

    CheckError(this->GetApplicationManager()->GetFileWatcher()->WatchFiles(
        scriptName, 
        CModuleConfiguration::GetWatchedFiles(context),
        CNodeApplicationManager::OnScriptModified, 
        this->GetApplicationManager(), 
        this));

    this->GetApplicationManager()->GetEventProvider()->Log(context, L"iisnode initialized a new node.js application", WINEVENT_LEVEL_INFO);

    return S_OK;
Error:

    this->GetApplicationManager()->GetEventProvider()->Log(context, L"iisnode failed to initialize a new node.js application", WINEVENT_LEVEL_ERROR);

    this->Cleanup();

    return hr;
}

PCWSTR CNodeApplication::GetConfigPath()
{
    return this->configPath;
}

HRESULT CNodeApplication::SetConfigPath(IHttpContext * context)
{
    HRESULT hr = S_OK;
    DWORD   dwConfigPathLen = wcslen(context->GetMetadata()->GetMetaPath());
    if(this->configPath != NULL)
    {
        delete [] this->configPath;
        this->configPath = NULL;
    }

    ErrorIf(NULL == (this->configPath = new WCHAR[dwConfigPathLen + 1]), E_OUTOFMEMORY);
    wcscpy(this->configPath, context->GetMetadata()->GetMetaPath());

Error:
    return hr;
}

PCWSTR CNodeApplication::GetScriptName()
{
    return this->scriptName;
}

CNodeApplicationManager* CNodeApplication::GetApplicationManager()
{
    return this->applicationManager;
}

HRESULT CNodeApplication::Dispatch(IHttpContext* context, IHttpEventProvider* pProvider, CNodeHttpStoredContext** ctx)
{
    HRESULT hr;

    CheckNull(ctx);
    CheckNull(context);
    CheckNull(pProvider);

    ErrorIf(NULL == (*ctx = new CNodeHttpStoredContext(this, this->GetApplicationManager()->GetEventProvider(), context)), ERROR_NOT_ENOUGH_MEMORY);	
    IHttpModuleContextContainer* moduleContextContainer = context->GetModuleContextContainer();
    moduleContextContainer->SetModuleContext(*ctx, this->GetApplicationManager()->GetModuleId());

    // increase the pending async opertation count; corresponding decrease happens in
    // CProtocolBridge::FinalizeResponseCore, possibly after several context switches
    (*ctx)->IncreasePendingAsyncOperationCount(); 

    // At this point ownership of the request is handed over to Dispach below, including error handling.
    // Therefore we don't propagate any errors up the stack.
    this->processManager->Dispatch(*ctx);

    return S_OK;

Error:	

    // on error, nodeContext need not be freed here as it will be deallocated through IHttpStoredContext when the request is finished

    return hr;
}

CNodeApplication* CNodeApplication::GetPeerApplication()
{
    return this->peerApplication;
}

void CNodeApplication::SetPeerApplication(CNodeApplication* peerApplication)
{
    this->peerApplication = peerApplication;
}

BOOL CNodeApplication::IsDebugger()
{
    return this->isDebugger;
}

BOOL CNodeApplication::IsDebuggee()
{
    return !this->IsDebugger() && NULL != this->GetPeerApplication();
}

BOOL CNodeApplication::IsDebugMode()
{
    return this->IsDebuggee() || this->IsDebugger();
}

NodeDebugCommand CNodeApplication::GetDebugCommand()
{
    return this->debugCommand;
}

HRESULT CNodeApplication::Recycle()
{
    return this->processManager->Recycle();
}

DWORD CNodeApplication::GetDebugPort()
{
    return this->debugPort;
}

DWORD CNodeApplication::GetActiveRequestCount()
{
    return this->processManager->GetActiveRequestCount();
}

DWORD CNodeApplication::GetProcessCount()
{
    return this->processManager->GetProcessCount();
}

void CNodeApplication::SetNeedsRecycling()
{
    this->needsRecycling = TRUE;
}

BOOL CNodeApplication::GetNeedsRecycling()
{
    return this->needsRecycling;
}

