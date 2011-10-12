@echo off

set appcmd=%systemroot%\system32\inetsrv\appcmd.exe
set apppool=iisnodetest
set site=iisnodetest
set port=31415
set node="%programfiles%\nodejs\node.exe"
if "%PROCESSOR_ARCHITECTURE%" neq "x86" set node="%programfiles(x86)%\nodejs\node.exe"
set www=
for /F %%I in ('dir /b /s %~dp0..\test.bat') do set www=%%~dI%%~pIwww
if "%log%" equ "" set log="%~dp0log.out"

if not exist %node% (
	echo FAILED. The node.exe was not found at %node%. Download a copy from http://nodejs.org.
	exit /b -1
)

if not exist %appcmd% (
	echo FAILED. The appcmd.exe IIS management tool was not found at %appcmd%. Make sure you have both IIS7 as well as IIS7 Management Tools installed.
	exit /b -1
)

if "%1" equ "/ns" exit /b 0

%appcmd% delete site %site% >> %log%
if %ERRORLEVEL% neq 0 if %ERRORLEVEL% neq 1168 (
	echo FAILED. Cannot delete site %site%.
	exit /b -1
)

%appcmd% delete apppool %apppool% >> %log%
if %ERRORLEVEL% neq 0 if %ERRORLEVEL% neq 1168 (
	echo FAILED. Cannot delete application pool %apppool%.
	exit /b -1
)

%appcmd% add apppool /name:%apppool% >> %log%
if %ERRORLEVEL% neq 0 (
	echo FAILED. Cannot create application pool %apppool%.
	exit /b -1
)

%appcmd% add site /name:%site% /physicalPath:"%www%" /bindings:http/*:%port%: >> %log%
if %ERRORLEVEL% neq 0 (
	echo FAILED. Cannot create site %site%.
	exit /b -1
)

%appcmd% set site %site% /[path='/'].applicationPool:%apppool% >> %log%
if %ERRORLEVEL% neq 0 (
	echo FAILED. Cannot configure site %site%.
	exit /b -1
)

%appcmd% start site %site% >> %log%
if %ERRORLEVEL% neq 0 (
	echo FAILED. Cannot start site %site%.
	exit /b -1
)

exit /b 0