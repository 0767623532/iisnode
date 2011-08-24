#include "precomp.h"

HRESULT CProtocolBridge::PostponeProcessing(CNodeHttpStoredContext* context, DWORD dueTime)
{
	CAsyncManager* async = context->GetNodeApplication()->GetApplicationManager()->GetAsyncManager();
	LARGE_INTEGER delay;
	delay.QuadPart = dueTime;
	delay.QuadPart *= -10000; // convert from ms to 100ns units

	return async->SetTimer(context->GetAsyncContext(), &delay);
}

HRESULT CProtocolBridge::SendEmptyResponse(CNodeHttpStoredContext* context, USHORT status, PCTSTR reason, HRESULT hresult)
{
	if (INVALID_HANDLE_VALUE != context->GetPipe())
	{
		CloseHandle(context->GetPipe());
		context->SetPipe(INVALID_HANDLE_VALUE);
	}

	context->SetHresult(hresult);
	context->SetRequestNotificationStatus(RQ_NOTIFICATION_FINISH_REQUEST);
	if (NULL != context->GetNodeProcess())
	{
		// there is no CNodeProcess assigned to the request yet - something failed before it was moved from the pending queue
		// to an active request queue of a specific process

		context->GetNodeProcess()->OnRequestCompleted(context);
	}

	CProtocolBridge::SendEmptyResponse(context->GetHttpContext(), status, reason, hresult);

	if (!context->GetSynchronous())
	{
		context->SetNextProcessor(NULL);
		return context->GetHttpContext()->PostCompletion(0);
	}

	return S_OK;
}

void CProtocolBridge::SendEmptyResponse(IHttpContext* httpCtx, USHORT status, PCTSTR reason, HRESULT hresult)
{
	if (!httpCtx->GetResponseHeadersSent())
	{
		httpCtx->GetResponse()->Clear();
		httpCtx->GetResponse()->SetStatus(status, reason, 0, hresult);
	}
}

HRESULT CProtocolBridge::InitiateRequest(CNodeHttpStoredContext* context)
{
	context->SetNextProcessor(CProtocolBridge::CreateNamedPipeConnection);
	CProtocolBridge::CreateNamedPipeConnection(S_OK, 0, context->InitializeOverlapped());

	return context->GetHresult(); // synchronous completion HRESULT
}

void WINAPI CProtocolBridge::CreateNamedPipeConnection(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);
	HANDLE pipe;

	ErrorIf(INVALID_HANDLE_VALUE == (pipe = CreateFile(
		ctx->GetNodeProcess()->GetNamedPipeName(),
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED,
		NULL)), 
		GetLastError());

	DWORD mode = PIPE_READMODE_BYTE;// | PIPE_NOWAIT;
	ErrorIf(!SetNamedPipeHandleState(
		pipe, 
		&mode,
		NULL,
		NULL), 
		GetLastError());

	ctx->SetPipe(pipe);
	ctx->GetNodeApplication()->GetApplicationManager()->GetAsyncManager()->AddAsyncCompletionHandle(pipe);
	
	CProtocolBridge::SendHttpRequestHeaders(ctx);

	return;

Error:

	DWORD retry = ctx->GetConnectionRetryCount();
	if (retry >= CModuleConfiguration::GetMaxNamedPipeConnectionRetry(ctx->GetHttpContext()))
	{
		if (hr == ERROR_PIPE_BUSY)
		{
			CProtocolBridge::SendEmptyResponse(ctx, 503, _T("Service Unavailable"), hr);
		}
		else
		{
			CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);
		}
	}
	else 
	{
		ctx->SetConnectionRetryCount(retry + 1);
		CProtocolBridge::PostponeProcessing(ctx, CModuleConfiguration::GetNamedPipeConnectionRetryDelay(ctx->GetHttpContext()));
	}

	return;
}

void CProtocolBridge::SendHttpRequestHeaders(CNodeHttpStoredContext* context)
{
	HRESULT hr;
	DWORD length;

	// request the named pipe to be closed by the server after the response is sent
	// since we are not reusing named pipe connections anyway, requesting the server 
	// to close it after sending the response allows the module to avoid parsing chunked responses
	// to detect end of response

	CheckError(context->GetHttpContext()->GetRequest()->SetHeader("Connection", "close", 5, TRUE));

	CheckError(CHttpProtocol::SerializeRequestHeaders(context->GetHttpContext(), context->GetBufferRef(), context->GetBufferSizeRef(), &length));

	context->SetNextProcessor(CProtocolBridge::SendHttpRequestHeadersCompleted);
	ErrorIf(!WriteFile(context->GetPipe(), context->GetBuffer(), length, NULL, context->InitializeOverlapped()), GetLastError());

	return;

Error:

	if (ERROR_IO_PENDING != hr)
	{
		CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void WINAPI CProtocolBridge::SendHttpRequestHeadersCompleted(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	CheckError(error);
	CProtocolBridge::ReadRequestBody(ctx);

	return;
Error:

	CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);

	return;
}

void CProtocolBridge::ReadRequestBody(CNodeHttpStoredContext* context)
{
	HRESULT hr;	
	DWORD bytesReceived;
	BOOL completionPending;

	context->SetNextProcessor(CProtocolBridge::ReadRequestBodyCompleted);
	CheckError(context->GetHttpContext()->GetRequest()->ReadEntityBody(context->GetBuffer(), context->GetBufferSize(), TRUE, &bytesReceived, &completionPending));
	if (!completionPending)
	{
		CProtocolBridge::ReadRequestBodyCompleted(S_OK, bytesReceived, context->GetOverlapped());
	}

	return;
Error:

	if (HRESULT_FROM_WIN32(ERROR_HANDLE_EOF) == hr)
	{
		CProtocolBridge::StartReadResponse(context);
	}
	else
	{
		CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), HRESULT_FROM_WIN32(hr));
	}

	return;
}

void WINAPI CProtocolBridge::ReadRequestBodyCompleted(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{	
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	if (S_OK == error && bytesTransfered > 0)
	{
		CProtocolBridge::SendRequestBody(ctx, bytesTransfered);
	}
	else if (ERROR_HANDLE_EOF == error)
	{		
		CProtocolBridge::StartReadResponse(ctx);
	}
	else 
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), error);
	}
}

void CProtocolBridge::SendRequestBody(CNodeHttpStoredContext* context, DWORD chunkLength)
{
	HRESULT hr;

	context->SetNextProcessor(CProtocolBridge::SendRequestBodyCompleted);
	ErrorIf(!WriteFile(context->GetPipe(), context->GetBuffer(), chunkLength, NULL, context->InitializeOverlapped()), GetLastError());
	
	return;

Error:

	if (ERROR_IO_PENDING != hr)
	{
		CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void WINAPI CProtocolBridge::SendRequestBodyCompleted(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	if (S_OK == error)
	{
		CProtocolBridge::ReadRequestBody(ctx);
	}
	else 
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), error);
	}
}

void CProtocolBridge::StartReadResponse(CNodeHttpStoredContext* context)
{
	context->SetDataSize(0);
	context->SetParsingOffset(0);
	context->SetNextProcessor(CProtocolBridge::ProcessResponseStatusLine);
	CProtocolBridge::ContinueReadResponse(context);
}

HRESULT CProtocolBridge::EnsureBuffer(CNodeHttpStoredContext* context)
{
	HRESULT hr;

	if (context->GetBufferSize() == context->GetDataSize())
	{
		// buffer is full

		if (context->GetParsingOffset() > 0)
		{
			// remove already parsed data from buffer by moving unprocessed data to the front; this will free up space at the end

			char* b = (char*)context->GetBuffer();
			memcpy(b, b + context->GetParsingOffset(), context->GetDataSize() - context->GetParsingOffset());
		}
		else 
		{
			// allocate more buffer memory

			DWORD* bufferLength = context->GetBufferSizeRef();
			void** buffer = context->GetBufferRef();

			DWORD quota = CModuleConfiguration::GetMaxRequestBufferSize(context->GetHttpContext());
			ErrorIf(*bufferLength >= quota, ERROR_NOT_ENOUGH_QUOTA);

			void* newBuffer;
			DWORD newBufferLength = *bufferLength * 2;
			if (newBufferLength > quota)
			{
				newBufferLength = quota;
			}

			ErrorIf(NULL == (newBuffer = context->GetHttpContext()->AllocateRequestMemory(newBufferLength)), ERROR_NOT_ENOUGH_MEMORY);
			memcpy(newBuffer, (char*)(*buffer) + context->GetParsingOffset(), context->GetDataSize() - context->GetParsingOffset());
			*buffer = newBuffer;
			*bufferLength = newBufferLength;
		}

		context->SetDataSize(context->GetDataSize() - context->GetParsingOffset());
		context->SetParsingOffset(0);
	}

	return S_OK;
Error:
	return hr;

}

void CProtocolBridge::ContinueReadResponse(CNodeHttpStoredContext* context)
{
	HRESULT hr;

	CheckError(CProtocolBridge::EnsureBuffer(context));

	ErrorIf(!ReadFile(
			context->GetPipe(), 
			(char*)context->GetBuffer() + context->GetDataSize(), 
			context->GetBufferSize() - context->GetDataSize(),
			NULL,
			context->InitializeOverlapped()),
		GetLastError());

	return;
Error:

	if (ERROR_IO_PENDING != hr)
	{
		if (context->GetResponseContentLength() == -1)
		{
			// connection termination with chunked transfer encoding indicates end of response
			// since we have sent Connection: close HTTP request header to node from SendHttpRequestHeaders

			// CR: narrow down this condition to orderly pipe closure
			CProtocolBridge::FinalizeResponse(context);
		}
		else
		{
			CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), hr);
		}
	}

	return;
}

void WINAPI CProtocolBridge::ProcessResponseStatusLine(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	CheckError(error);
	ctx->SetDataSize(ctx->GetDataSize() + bytesTransfered);
	CheckError(CHttpProtocol::ParseResponseStatusLine(ctx));
	ctx->SetNextProcessor(CProtocolBridge::ProcessResponseHeaders);
	CProtocolBridge::ProcessResponseHeaders(S_OK, 0, ctx->GetOverlapped());

	return;
Error:

	if (ERROR_MORE_DATA == hr)
	{
		CProtocolBridge::ContinueReadResponse(ctx);
	}
	else
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void WINAPI CProtocolBridge::ProcessResponseHeaders(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);
	PCSTR contentLength;
	USHORT contentLengthLength;

	CheckError(error);
	ctx->SetDataSize(ctx->GetDataSize() + bytesTransfered);
	CheckError(CHttpProtocol::ParseResponseHeaders(ctx));

	contentLength = ctx->GetHttpContext()->GetResponse()->GetHeader(HttpHeaderContentLength, &contentLengthLength);
	if (0 == contentLengthLength)
	{
		ctx->SetResponseContentLength(-1);
	}
	else
	{
		LONGLONG length = 0;
		for (int i = 0; i < contentLengthLength; i++)
		{
			length = length * 10 + contentLength[i] - '0';
		}
		ctx->SetResponseContentLength(length);
	}
	ctx->SetNextProcessor(CProtocolBridge::ProcessResponseBody);
	CProtocolBridge::ProcessResponseBody(S_OK, 0, ctx->GetOverlapped());

	return;
Error:

	if (ERROR_MORE_DATA == hr)
	{
		CProtocolBridge::ContinueReadResponse(ctx);
	}
	else
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void WINAPI CProtocolBridge::ProcessResponseBody(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);
	HTTP_DATA_CHUNK* chunk;
	DWORD bytesSent;
	BOOL completionExpected;
	
	if (S_OK != error)
	{
		if (ctx->GetResponseContentLength() == -1)
		{
			// connection termination with chunked transfer encoding indicates end of response
			// since we have sent Connection: close HTTP request header to node from SendHttpRequestHeaders

			// CR: check the other commend for finalizing response
			CProtocolBridge::FinalizeResponse(ctx);
		}
		else
		{
			CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), error);
		}

		return;
	}

	ctx->SetDataSize(ctx->GetDataSize() + bytesTransfered);

	if (ctx->GetDataSize() > ctx->GetParsingOffset())
	{
		// send body data to client

		// CR: consider using malloc here (memory can be released after Flush)

		ErrorIf(NULL == (chunk = (HTTP_DATA_CHUNK*) ctx->GetHttpContext()->AllocateRequestMemory(sizeof HTTP_DATA_CHUNK)), ERROR_NOT_ENOUGH_MEMORY);
		chunk->DataChunkType = HttpDataChunkFromMemory;
		chunk->FromMemory.BufferLength = ctx->GetDataSize() - ctx->GetParsingOffset();
		ErrorIf(NULL == (chunk->FromMemory.pBuffer = ctx->GetHttpContext()->AllocateRequestMemory(chunk->FromMemory.BufferLength)), ERROR_NOT_ENOUGH_MEMORY);
		memcpy(chunk->FromMemory.pBuffer, (char*)ctx->GetBuffer() + ctx->GetParsingOffset(), chunk->FromMemory.BufferLength);

		ctx->SetDataSize(0);
		ctx->SetParsingOffset(0);
		ctx->SetNextProcessor(CProtocolBridge::SendResponseBodyCompleted);

		CheckError(ctx->GetHttpContext()->GetResponse()->WriteEntityChunks(
			chunk,
			1,
			TRUE,
			ctx->GetResponseContentLength() == -1 || ctx->GetResponseContentLength() > (ctx->GetResponseContentTransmitted() + chunk->FromMemory.BufferLength),
			&bytesSent,
			&completionExpected));
		
		if (!completionExpected)
		{
			CProtocolBridge::SendResponseBodyCompleted(S_OK, chunk->FromMemory.BufferLength, ctx->GetOverlapped());
		}
	}
	else if (-1 == ctx->GetResponseContentLength() || ctx->GetResponseContentLength() > ctx->GetResponseContentTransmitted())
	{
		// read more body data

		CProtocolBridge::ContinueReadResponse(ctx);
	}
	else
	{
		// finish request

		CProtocolBridge::FinalizeResponse(ctx);
	}

	return;
Error:

	CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);

	return;
}

void WINAPI CProtocolBridge::SendResponseBodyCompleted(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);
	DWORD bytesSent;
	BOOL completionExpected = FALSE;

	CheckError(error);
	ctx->SetResponseContentTransmitted(ctx->GetResponseContentTransmitted() + bytesTransfered);

	if (ctx->GetResponseContentLength() == -1 || ctx->GetResponseContentLength() > ctx->GetResponseContentTransmitted())
	{
		if (ctx->GetResponseContentLength() == -1)
		{
			// Flush chunked responses. 
			// TODO, tjanczuk, is flushing chunked responses requried

			ctx->SetNextProcessor(CProtocolBridge::ContinueProcessResponseBodyAfterPartialFlush);
			ctx->GetHttpContext()->GetResponse()->Flush(TRUE, TRUE, &bytesSent, &completionExpected);
		}

		if (!completionExpected)
		{
			CProtocolBridge::ContinueProcessResponseBodyAfterPartialFlush(S_OK, 0, ctx->GetOverlapped());
		}
	}
	else
	{
		CProtocolBridge::FinalizeResponse(ctx);
	}

	return;
Error:

	CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);

	return;
}

void WINAPI CProtocolBridge::ContinueProcessResponseBodyAfterPartialFlush(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	CheckError(error);	
	ctx->SetNextProcessor(CProtocolBridge::ProcessResponseBody);
	CProtocolBridge::ContinueReadResponse(ctx);

	return;
Error:

	CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);
	
	return;
}

void CProtocolBridge::FinalizeResponse(CNodeHttpStoredContext* context)
{
	CloseHandle(context->GetPipe());
	context->SetPipe(INVALID_HANDLE_VALUE);

	context->GetNodeProcess()->OnRequestCompleted(context);
	context->SetRequestNotificationStatus(RQ_NOTIFICATION_CONTINUE);
	if (!context->GetSynchronous())
	{
		context->SetNextProcessor(NULL);
		context->GetHttpContext()->PostCompletion(0);
	}
}