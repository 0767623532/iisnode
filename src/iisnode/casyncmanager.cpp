#include "precomp.h"

extern RtlNtStatusToDosError pRtlNtStatusToDosError;

CAsyncManager::CAsyncManager()
	: threads(NULL), threadCount(0), completionPort(NULL)
{
}

CAsyncManager::~CAsyncManager()
{
	this->Terminate();
}

HRESULT CAsyncManager::Initialize(IHttpContext* context)
{
	HRESULT hr;
	DWORD initializedThreads = 0;

	ErrorIf(NULL != this->threads, ERROR_INVALID_OPERATION);

	this->threadCount = CModuleConfiguration::GetAsyncCompletionThreadCount(context);

	ErrorIf(NULL == (this->completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, this->threadCount)), GetLastError());		
	
	ErrorIf(NULL == (this->threads = new HANDLE[this->threadCount]), ERROR_NOT_ENOUGH_MEMORY);
	
	for (initializedThreads = 0; initializedThreads < this->threadCount; initializedThreads++)
	{
		ErrorIf((HANDLE)-1L == (this->threads[initializedThreads] = (HANDLE) _beginthreadex(NULL, 0, CAsyncManager::Worker, this, 0, NULL)), ERROR_NOT_ENOUGH_MEMORY);
	}

	return S_OK;
Error:

	this->threadCount = initializedThreads;
	this->Terminate();

	return hr;
}

HRESULT CAsyncManager::AddAsyncCompletionHandle(HANDLE handle)
{
	HRESULT hr;

	CheckNull(handle);
	ErrorIf(NULL == CreateIoCompletionPort(handle, this->completionPort, NULL, 0), GetLastError());

	return S_OK;
Error:
	return hr;
}

HRESULT CAsyncManager::Terminate()
{
	HRESULT hr;

	if (NULL != this->threads)
	{
		for (DWORD i = 0; i < this->threadCount; i++)
		{
			ErrorIf(0 == PostQueuedCompletionStatus(this->completionPort, 0, -1L, NULL), ERROR_OPERATION_ABORTED);
		}

		WaitForMultipleObjects(this->threadCount, this->threads, TRUE, INFINITE);
		for (DWORD i = 0; i < this->threadCount; i++)
		{
			CloseHandle(this->threads[i]);
		}

		delete this->threads;
		this->threads = NULL;
		this->threadCount = 0;
	}

	if (NULL != this->completionPort)
	{
		CloseHandle(this->completionPort);
		this->completionPort = NULL;
	}

	return S_OK;
Error:
	return hr;
}

HRESULT CAsyncManager::SetTimer(ASYNC_CONTEXT* context, LARGE_INTEGER* dueTime)
{
	HRESULT hr;

	CheckNull(context);
	CheckNull(dueTime);
	ErrorIf(NULL != context->timer, ERROR_INVALID_OPERATION);

	context->dueTime = *dueTime;
	ErrorIf(NULL == (context->timer = CreateWaitableTimer(NULL, TRUE, NULL)), GetLastError());
	ErrorIf(0 == PostQueuedCompletionStatus(this->completionPort, 0, -2L, (LPOVERLAPPED)context), ERROR_OPERATION_ABORTED);	

	return S_OK;
Error:
	return hr;
}

unsigned int WINAPI CAsyncManager::Worker(void* arg)
{
	HANDLE completionPort = ((CAsyncManager*)arg)->completionPort;
	ASYNC_CONTEXT* ctx;
	DWORD error;
	OVERLAPPED_ENTRY entry;
	ULONG removed;

	while (true) 
	{
		error = ERROR_SUCCESS;
		if (!GetQueuedCompletionStatusEx(completionPort, &entry, 1, &removed, INFINITE, TRUE))
		{
			// No I/O completion packets were dequeued, continue waiting.
			continue;
		}
		
		if (removed == 1)
		{
			if (0L == entry.lpCompletionKey 
				&& NULL != (ctx = (ASYNC_CONTEXT*)entry.lpOverlapped) 
				&& NULL != ctx->completionProcessor) // regular IO completion - invoke custom processor
			{
				error = (entry.lpOverlapped->Internal == STATUS_SUCCESS) ? ERROR_SUCCESS 
					: pRtlNtStatusToDosError(entry.lpOverlapped->Internal);
				ctx = (ASYNC_CONTEXT*)entry.lpOverlapped;
				ctx->synchronous = FALSE;
				ctx->completionProcessor(
					(0 == entry.dwNumberOfBytesTransferred && ERROR_SUCCESS == error) ? ERROR_NO_DATA : error, 
					entry.dwNumberOfBytesTransferred, 
					(LPOVERLAPPED)ctx);
			}
			else if (-1L == entry.lpCompletionKey) // shutdown initiated from Terminate
			{
				break;
			}
			else if (-2L == entry.lpCompletionKey) // setup of an alertable wait state timer from SetTimer
			{
				ctx = (ASYNC_CONTEXT*)entry.lpOverlapped;
				SetWaitableTimer(ctx->timer, &ctx->dueTime, 0, CAsyncManager::OnTimer, ctx, FALSE);
			}
			else if (-3L == entry.lpCompletionKey) // continuation initiated form PostContinuation
			{
				((ContinuationCallback)entry.lpOverlapped)((void*)entry.dwNumberOfBytesTransferred);
			}
		}
	}

	return 0;
}

void APIENTRY CAsyncManager::OnTimer(LPVOID lpArgToCompletionRoutine, DWORD dwTimerLowValue, DWORD dwTimerHighValue)
{
	ASYNC_CONTEXT* ctx = (ASYNC_CONTEXT*)lpArgToCompletionRoutine;

	CloseHandle(ctx->timer);
	ctx->timer = NULL;

	if (NULL != ctx && NULL != ctx->completionProcessor)
	{
		ctx->synchronous = FALSE;
		ctx->completionProcessor(S_OK, 0, (LPOVERLAPPED)ctx);
	}
}

HRESULT CAsyncManager::PostContinuation(ContinuationCallback continuation, void* data)
{
	HRESULT hr;

	CheckNull(continuation);
	ErrorIf(0 == PostQueuedCompletionStatus(this->completionPort, (DWORD)data, -3L, (LPOVERLAPPED)continuation), ERROR_OPERATION_ABORTED);

	return S_OK;
Error:
	return hr;
}
