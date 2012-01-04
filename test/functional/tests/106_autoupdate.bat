setlocal 

copy /y %www%\106_autoupdate\hello_first.js %www%\106_autoupdate\hello.js
if %ERRORLEVEL% neq 0 exit /b -1

call %this%scripts\runNodeTest.bat parts\106_autoupdate_first.js
if %ERRORLEVEL% neq 0 del /q %www%\106_autoupdate\hello.js & exit /b -1

copy /y %www%\106_autoupdate\hello_second.js %www%\106_autoupdate\hello.js
if %ERRORLEVEL% neq 0 del /q %www%\106_autoupdate\hello.js & exit /b -1

timeout /T 5 /NOBREAK

call %this%scripts\runNodeTest.bat parts\106_autoupdate_second.js
if %ERRORLEVEL% neq 0 del /q %www%\106_autoupdate\hello.js & exit /b -1

del /q %www%\106_autoupdate\hello.js

exit /b 0

endlocal