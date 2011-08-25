Hosting node.js applications in IIS on Windows
===

**Prerequisites**

- Windows
- IIS 7.x with IIS Management Tools
- Latest node.exe Windows build from [nodejs.org](http://nodejs.org/#download) saved to %systemdrive%\node directory
- [Visual Studio C++ Express](http://www.microsoft.com/visualstudio/en-us/products/2010-editions/visual-cpp-express) (development only)
- [IIS 7 header files from Windows SDK](http://msdn.microsoft.com/en-us/windows/bb980924) (development only)

**Building**

    msbuild src\iisnode\iisnode.sln
    
**Installing after build**

    build\debug\install.bat
    
on 64-bit Windows systems, make sure the application pool associated with Default Web Site allows running 32 bit code

**Installing from a download**

- [Download and unzip desired build](https://github.com/tjanczuk/iisnode/archives/master)
- call install.bat
- on 64-bit Windows systems, make sure the application pool associated with Default Web Site allows running 32 bit code

**Samples**

    http://localhost/node

**Resources & documentation**

- https://github.com/tjanczuk/iisnode/wiki
- http://tomasz.janczuk.org