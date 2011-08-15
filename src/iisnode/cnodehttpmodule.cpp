#include "precomp.h"

CNodeHttpModule::CNodeHttpModule(CNodeApplicationManager* applicationManager)
	: applicationManager(applicationManager)
{
}

REQUEST_NOTIFICATION_STATUS CNodeHttpModule::OnExecuteRequestHandler(
	IN IHttpContext* pHttpContext, 
	IN IHttpEventProvider* pProvider)
{
	HRESULT hr;

	CheckError(this->applicationManager->Dispatch(pHttpContext, pProvider));

	return RQ_NOTIFICATION_PENDING;
Error:

	if (ERROR_NOT_ENOUGH_QUOTA == hr)
	{
		CProtocolBridge::SendEmptyResponse(pHttpContext, 503, _T("Service Unavailable"), hr, FALSE);
	}
	else
	{
		CProtocolBridge::SendEmptyResponse(pHttpContext, 500, _T("Internal Server Error"), hr, FALSE);
	}

	return RQ_NOTIFICATION_FINISH_REQUEST;
}

REQUEST_NOTIFICATION_STATUS CNodeHttpModule::OnAsyncCompletion(
	IHttpContext* pHttpContext, DWORD dwNotification, BOOL fPostNotification, IHttpEventProvider* pProvider, IHttpCompletionInfo* pCompletionInfo)
{
	if (NULL != pCompletionInfo && NULL != pHttpContext)
	{
		CNodeHttpStoredContext* ctx = (CNodeHttpStoredContext*)pHttpContext->GetModuleContextContainer()->GetModuleContext(this->applicationManager->GetModuleId());
		ctx->SetSynchronous(TRUE);
		ASYNC_CONTEXT* async = ctx->GetAsyncContext();
		async->completionProcessor(pCompletionInfo->GetCompletionStatus(), pCompletionInfo->GetCompletionBytes(), ctx->GetOverlapped());
		return ctx->GetRequestNotificationStatus();
	}

	return RQ_NOTIFICATION_CONTINUE;
}
