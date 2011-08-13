#ifndef __UTILS_H__
#define __UTILS_H__

#define ErrorIf(expr, hresult) if (expr) { hr = hresult; goto Error; }

#define CheckNull(expr) ErrorIf(NULL == expr, ERROR_INVALID_PARAMETER)

#define CheckError(hresult) { HRESULT tmp_hr = hresult; if (S_OK != tmp_hr) { hr = tmp_hr; goto Error; } }

#define ENTER_CS(cs) EnterCriticalSection(&cs); __try {

#define LEAVE_CS(cs) } __finally { LeaveCriticalSection(&cs); }

#endif