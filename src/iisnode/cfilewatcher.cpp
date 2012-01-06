#include "precomp.h"

CFileWatcher::CFileWatcher()
	: completionPort(NULL), worker(NULL), directories(NULL), uncFileSharePollingInterval(0)
{
	InitializeCriticalSection(&this->syncRoot);
}

CFileWatcher::~CFileWatcher()
{
	if (NULL != this->worker)
	{
		PostQueuedCompletionStatus(this->completionPort, 0, -1L, NULL);
		WaitForSingleObject(this->worker, INFINITE);
		CloseHandle(this->worker);
		this->worker = NULL;
	}

	if (NULL != this->completionPort)
	{
		CloseHandle(this->completionPort);
		this->completionPort = NULL;
	}

	while (NULL != this->directories)
	{
		WatchedDirectory* currentDirectory = this->directories;
		CloseHandle(currentDirectory->watchHandle);
		delete [] currentDirectory->directoryName;
		while (NULL != currentDirectory->files)
		{
			WatchedFile* currentFile = currentDirectory->files;
			delete [] currentFile->fileName;
			currentDirectory->files = currentFile->next;
			delete currentFile;
		}
		this->directories = currentDirectory->next;
		delete currentDirectory;
	}

	DeleteCriticalSection(&this->syncRoot);
}

HRESULT CFileWatcher::Initialize(IHttpContext* context)
{
	HRESULT hr;

	ErrorIf(NULL == (this->completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1)), GetLastError());		
	this->uncFileSharePollingInterval = CModuleConfiguration::GetUNCFileChangesPollingInterval(context);

	return S_OK;
Error:

	if (NULL != this->completionPort)
	{
		CloseHandle(this->completionPort);
		this->completionPort = NULL;
	}

	return hr;
}

HRESULT CFileWatcher::WatchFiles(PCWSTR mainFileName, PCWSTR watchedFiles, FileModifiedCallback callback, CNodeApplicationManager* manager, CNodeApplication* application)
{
	HRESULT hr;
	WCHAR fileOnly[_MAX_FNAME];
	WCHAR ext[_MAX_EXT];
	WCHAR* directoryName = NULL;
	DWORD fileNameLength;
	DWORD fileNameOnlyLength;
	DWORD directoryLength;	
	BOOL unc;
	BOOL wildcard;
	PWSTR startSubdirectory;
	PWSTR startFile;
	PWSTR endFile;
	WatchedDirectory* directory;
	WatchedFile* file;

	CheckNull(mainFileName);
	CheckNull(watchedFiles);

	// create and normalize a copy of directory name, determine if it is UNC share

	fileNameLength = wcslen(mainFileName);
	ErrorIf(0 != _wsplitpath_s(mainFileName, NULL, 0, NULL, 0, fileOnly, _MAX_FNAME, ext, _MAX_EXT), ERROR_INVALID_PARAMETER);	
	fileNameOnlyLength = wcslen(fileOnly) + wcslen(ext);	
	directoryLength = fileNameLength - fileNameOnlyLength; 
	ErrorIf(NULL == (directoryName = new WCHAR[directoryLength + 8]), ERROR_NOT_ENOUGH_MEMORY); // pessimistic length after normalization with prefix \\?\UNC\ 

	if (fileNameLength > 8 && 0 == memcmp(mainFileName, L"\\\\?\\UNC\\", 8 * sizeof WCHAR))
	{
		// normalized UNC path
		unc = TRUE;
		memcpy(directoryName, mainFileName, directoryLength * sizeof WCHAR);
		directoryName[directoryLength] = L'\0';
	}
	else if (fileNameLength > 4 && 0 == memcmp(mainFileName, L"\\\\?\\", 4 * sizeof WCHAR))
	{
		// normalized local file
		unc = FALSE;
		memcpy(directoryName, mainFileName, directoryLength * sizeof WCHAR);
		directoryName[directoryLength] = L'\0';
	}
	else if (fileNameLength > 2 && 0 == memcmp(mainFileName, L"\\\\", 2 * sizeof(WCHAR)))
	{
		// not normalized UNC path
		unc = TRUE;
		wcscpy(directoryName, L"\\\\?\\UNC\\");
		memcpy(directoryName + 8, mainFileName + 2, (directoryLength - 2) * sizeof WCHAR);
		directoryName[8 + directoryLength - 2] = L'\0';
	}
	else
	{
		// not normalized local file
		unc = FALSE;
		wcscpy(directoryName, L"\\\\?\\");
		memcpy(directoryName + 4, mainFileName, directoryLength * sizeof WCHAR);
		directoryName[4 + directoryLength] = L'\0';	
	}

	directoryLength = wcslen(directoryName);

	ENTER_CS(this->syncRoot)

	// parse watchedFiles and create a file listener for each of the files

	startFile = (PWSTR)watchedFiles;
	do {
		endFile = startSubdirectory = startFile;
		wildcard = FALSE;
		while (*endFile && *endFile != L';')
		{
			wildcard |= *endFile == L'*' || *endFile == L'?';
			if (*endFile == L'\\')
			{
				startFile = endFile + 1;
			}

			endFile++;
		}

		if (startFile != endFile)
		{
			if (S_OK != (hr = this->WatchFile(directoryName, directoryLength, unc, startSubdirectory, startFile, endFile, wildcard)))
			{
				// still under lock remove file watch entries that were just created, then do regular cleanup

				this->RemoveWatch(NULL);
				CheckError(hr);
			}
		}

		startFile = endFile + 1;

	} while(*endFile);

	// update temporary entries with application and callback pointers

	directory = this->directories;
	while (NULL != directory)
	{
		file = directory->files;
		while (NULL != file)
		{
			if (NULL == file->application)
			{
				file->application = application;
				file->manager = manager;
				file->callback = callback;
			}

			file = file->next;
		}

		directory = directory->next;
	}

	LEAVE_CS(this->syncRoot)

	delete [] directoryName;

	return S_OK;

Error:

	if (NULL != directoryName)
	{
		delete [] directoryName;
		directoryName = NULL;
	}


	return hr;
}

HRESULT CFileWatcher::GetWatchedFileTimestamp(WatchedFile* file, FILETIME* timestamp)
{
	HRESULT hr;
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	WIN32_FIND_DATAW findData;
	HANDLE foundFile = INVALID_HANDLE_VALUE;

	if (file->wildcard)
	{
		// a timestamp of a wildcard watched file is the XOR of the timestamps of all matching files and their names and sizes;
		// that way if any of the matching files changes, or matching files are added or removed, the timestamp will change as well

		RtlZeroMemory(timestamp, sizeof FILETIME);
		foundFile = FindFirstFileW(file->fileName, &findData);
		if (INVALID_HANDLE_VALUE != foundFile)
		{
			do
			{
				if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				{
					timestamp->dwHighDateTime ^= findData.ftLastWriteTime.dwHighDateTime ^ findData.nFileSizeHigh;
					timestamp->dwLowDateTime ^= findData.ftLastWriteTime.dwLowDateTime ^ findData.nFileSizeLow;
					WCHAR* current = findData.cFileName;
					while (*current)
					{
						timestamp->dwLowDateTime ^= *current;
						current++;
					}
				}
			} while (FindNextFileW(foundFile, &findData));

			ErrorIf(ERROR_NO_MORE_FILES != (hr = GetLastError()), hr);
			FindClose(foundFile);
			foundFile = NULL;
		}
	}
	else
	{
		ErrorIf(!GetFileAttributesExW(file->fileName, GetFileExInfoStandard, &attributes), GetLastError());
		memcpy(timestamp, &attributes.ftLastWriteTime, sizeof attributes.ftLastWriteTime);
	}

	return S_OK;
Error:

	if (INVALID_HANDLE_VALUE != foundFile)
	{
		FindClose(foundFile);
		foundFile = INVALID_HANDLE_VALUE;
	}

	return hr;
}

HRESULT CFileWatcher::WatchFile(PCWSTR directoryName, DWORD directoryNameLength, BOOL unc, PCWSTR startSubdirectoryName, PCWSTR startFileName, PCWSTR endFileName, BOOL wildcard)
{
	HRESULT hr;
	WatchedFile* file;
	WatchedDirectory* directory;
	WatchedDirectory* newDirectory;

	// allocate new WatchedFile, get snapshot of the last write time

	ErrorIf(NULL == (file = new WatchedFile), ERROR_NOT_ENOUGH_MEMORY);
	RtlZeroMemory(file, sizeof WatchedFile);
	ErrorIf(NULL == (file->fileName = new WCHAR[directoryNameLength + endFileName - startSubdirectoryName + 1]), ERROR_NOT_ENOUGH_MEMORY);
	wcscpy(file->fileName, directoryName);
	memcpy((void*)(file->fileName + directoryNameLength), startSubdirectoryName, (endFileName - startSubdirectoryName) * sizeof WCHAR);
	file->fileName[directoryNameLength + endFileName - startSubdirectoryName] = L'\0';
	file->unc = unc;
	file->wildcard = wildcard;
	this->GetWatchedFileTimestamp(file, &file->lastWrite);

	// find matching directory watcher entry

	directory = this->directories;
	while (NULL != directory)
	{
		if (0 == wcsncmp(directory->directoryName, directoryName, directoryNameLength)
			&& (startFileName == startSubdirectoryName 
			    || 0 == wcsncmp(directory->directoryName + directoryNameLength, startSubdirectoryName, startFileName - startSubdirectoryName)))
		{
			break;
		}

		directory = directory->next;
	}

	// if directory watcher not found, create one

	if (NULL == directory)
	{
		ErrorIf(NULL == (newDirectory = new WatchedDirectory), ERROR_NOT_ENOUGH_MEMORY);	
		RtlZeroMemory(newDirectory, sizeof WatchedDirectory);
		ErrorIf(NULL == (newDirectory->directoryName = new WCHAR[directoryNameLength + startFileName - startSubdirectoryName + 1]), ERROR_NOT_ENOUGH_MEMORY);
		wcscpy(newDirectory->directoryName, directoryName);
		if (startFileName > startSubdirectoryName)
		{
			wcsncat(newDirectory->directoryName, startSubdirectoryName, startFileName - startSubdirectoryName);
		}

		newDirectory->files = file;

		ErrorIf(INVALID_HANDLE_VALUE == (newDirectory->watchHandle = CreateFileW(
			newDirectory->directoryName,           
			FILE_LIST_DIRECTORY,    
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			NULL, 
			OPEN_EXISTING,         
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			NULL)),
			GetLastError());

		ErrorIf(NULL == CreateIoCompletionPort(newDirectory->watchHandle, this->completionPort, (ULONG_PTR)newDirectory, 0), 
			GetLastError());

		ErrorIf(0 == ReadDirectoryChangesW(
			newDirectory->watchHandle,
			&newDirectory->info,
			sizeof newDirectory->info,
			FALSE,
			FILE_NOTIFY_CHANGE_LAST_WRITE,
			NULL,
			&newDirectory->overlapped,
			NULL),
			GetLastError());

		if (NULL == this->worker)
		{
			// no watchers exist yet, start the watcher thread

			ErrorIf(NULL == (this->worker = (HANDLE)_beginthreadex(
				NULL, 
				4096, 
				CFileWatcher::Worker, 
				this, 
				0, 
				NULL)), 
				ERROR_NOT_ENOUGH_MEMORY);
		}

		newDirectory->next = this->directories;
		this->directories = newDirectory;
		newDirectory = NULL;
	}
	else
	{
		file->next = directory->files;
		directory->files = file;
		file = NULL;
	}	

	return S_OK;
Error:

	if (NULL != newDirectory)
	{
		if (NULL != newDirectory->directoryName)
		{
			delete [] newDirectory->directoryName;
		}

		if (NULL != newDirectory->watchHandle)
		{
			CloseHandle(newDirectory->watchHandle);
		}

		delete newDirectory;
	}

	if (NULL != file)
	{
		if (NULL != file->fileName)
		{
			delete [] file->fileName;
		}

		delete file;
	}

	return hr;
}

HRESULT CFileWatcher::RemoveWatch(CNodeApplication* application)
{
	ENTER_CS(this->syncRoot)

	WatchedDirectory* directory = this->directories;
	WatchedDirectory* previousDirectory = NULL;
	while (directory)
	{
		WatchedFile* file = directory->files;
		WatchedFile* previousFile = NULL;
		while (file && file->application != application)
		{
			previousFile = file;
			file = file->next;
		}

		if (file)
		{
			delete [] file->fileName;
			if (previousFile)
			{
				previousFile->next = file->next;
			}
			else
			{
				directory->files = file->next;
			}

			delete file;

			if (!directory->files)
			{
				delete [] directory->directoryName;
				CloseHandle(directory->watchHandle);

				if (previousDirectory)
				{
					previousDirectory->next = directory->next;
				}
				else
				{
					this->directories = directory->next;
				}

				delete directory;
			}

			break;
		}

		previousDirectory = directory;
		directory = directory->next;
	}

	LEAVE_CS(this->syncRoot)

	return S_OK;
}

unsigned int CFileWatcher::Worker(void* arg)
{
	CFileWatcher* watcher = (CFileWatcher*)arg;
	DWORD error;
	DWORD bytes;
	ULONG_PTR key;
	LPOVERLAPPED overlapped;

	while (TRUE)
	{
		error = S_OK;
		if (!GetQueuedCompletionStatus(
			watcher->completionPort, &bytes, &key, &overlapped, watcher->uncFileSharePollingInterval))
		{
			error = GetLastError();
		}

		if (-1L == key) // terminate thread
		{
			break;
		}
		else if (S_OK == error) // a change in registered directory detected (local file system)
		{
			WatchedDirectory* directory = (WatchedDirectory*)key;
			
			ENTER_CS(watcher->syncRoot)

			// make sure the directory is still watched

			WatchedDirectory* current = watcher->directories;
			while (current && current != directory)
				current = current->next;

			if (current)
			{
				watcher->ScanDirectory(current, FALSE);

				// make sure the directory is still watched - it could have been removed by a recursive call to RemoveWatch

				current = watcher->directories;
				while (current && current != directory)
					current = current->next;

				if (current)
				{
					RtlZeroMemory(&current->overlapped, sizeof current->overlapped);
					ReadDirectoryChangesW(
						current->watchHandle,
						&current->info,
						sizeof directory->info,
						FALSE,
						FILE_NOTIFY_CHANGE_LAST_WRITE,
						NULL,
						&current->overlapped,
						NULL);
				}
			}

			LEAVE_CS(watcher->syncRoot)
		}
		else // timeout - scan all registered UNC files for changes
		{
			ENTER_CS(watcher->syncRoot)

			WatchedDirectory* current = watcher->directories;
			while (current)
			{
				if (watcher->ScanDirectory(current, TRUE))
					break;
				current = current->next;
			}

			LEAVE_CS(watcher->syncRoot)
		}		
	}

	return 0;
}

BOOL CFileWatcher::ScanDirectory(WatchedDirectory* directory, BOOL unc)
{
	WatchedFile* file = directory->files;
	FILETIME timestamp;

	while (file)
	{
		if (unc == file->unc) 
		{
			if (S_OK == CFileWatcher::GetWatchedFileTimestamp(file, &timestamp)
				&& 0 != memcmp(&timestamp, &file->lastWrite, sizeof FILETIME))
			{
				memcpy(&file->lastWrite, &timestamp, sizeof FILETIME);
				file->callback(file->manager, file->application);
				return TRUE;
			}
		}
		file = file->next;
	}

	return FALSE;
}
