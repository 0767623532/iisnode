#ifndef __CNODEHTTPSTOREDCONTEXT_H__
#define __CNODEHTTPSTOREDCONTEXT_H__

class CNodeApplication;

class CNodeHttpStoredContext : public IHttpStoredContext
{
private:

	CNodeApplication* nodeApplication;
	IHttpContext* context;
	ASYNC_CONTEXT asyncContext;

public:

	// Context is owned by the caller
	CNodeHttpStoredContext(CNodeApplication* nodeApplication, IHttpContext* context);
	~CNodeHttpStoredContext();

	IHttpContext* GetHttpContext();
	CNodeApplication* GetNodeApplication();
	LPOVERLAPPED GetOverlapped();

	void SetNextProcessor(LPOVERLAPPED_COMPLETION_ROUTINE processor);	

	virtual void CleanupStoredContext();
};

#endif