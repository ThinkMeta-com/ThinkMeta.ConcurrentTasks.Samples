@echo off
rem Builds cjpegli-oracle.exe (bin\cjpegli.exe): the unmodified jpegli of ..\jpegli
rem (google/jpegli @ 031a007) with its unmodified tools\cjpegli.cc, plus --target to choose the
rem SIMD target. Run it from a "Developer Command Prompt for VS 2026" (cmake on the PATH).
rem Output: build\Release\cjpegli-oracle.exe
rem Note: zlib's CMake renames ..\jpegli\third_party\zlib\zconf.h to zconf.h.included, so git
rem shows that file as changed after a build ("git checkout -- ." restores it).
setlocal
set "HERE=%~dp0"
set "JPEGLI=%HERE%..\jpegli"
rem jpegli's CMake takes a bundled third_party library only if its folder has a .git entry (a git
rem submodule); git does not keep these entries in this repository, so they are created here.
for %%d in (highway lcms libjpeg-turbo libpng skcms zlib) do (
    if not exist "%JPEGLI%\third_party\%%d\.git" type nul > "%JPEGLI%\third_party\%%d\.git"
)
cmake -S "%HERE%." -B "%HERE%build" -G "Visual Studio 18 2026" -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || exit /b 1
cmake --build "%HERE%build" --config Release --target cjpegli-oracle || exit /b 1
echo built: %HERE%build\Release\cjpegli-oracle.exe
