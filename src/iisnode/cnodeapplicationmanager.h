#ifndef __CNODEAPPLICATIONMANAGER_H__
#define __CNODEAPPLICATIONMANAGER_H__

class CNodeApplication;
class CAsyncManager;

class CNodeApplicationManager
{
private:

	typedef struct _NodeApplicationEntry {
		CNodeApplication* nodeApplication;
		struct _NodeApplicationEntry* next;
	} NodeApplicationEntry;

	IHttpServer* server;
	HTTP_MODULE_ID moduleId;
	NodeApplicationEntry* applications;
	CRITICAL_SECTION syncRoot;
	CAsyncManager* asyncManager;

	HRESULT GetOrCreateNodeApplication(IHttpContext* context, CNodeApplication** application);
	HRESULT GetOrCreateNodeApplicationCore(PCWSTR physicalPath, CNodeApplication** application);
	CNodeApplication* TryGetExistingNodeApplication(PCWSTR physicalPath);

public:

	CNodeApplicationManager(IHttpServer* server, HTTP_MODULE_ID moduleId); 
	~CNodeApplicationManager();

	IHttpServer* GetHttpServer();
	HTTP_MODULE_ID GetModuleId();
	CAsyncManager* GetAsyncManager();

	HRESULT Initialize();
	HRESULT Dispatch(IHttpContext* context, IHttpEventProvider* pProvider);
};

#endif