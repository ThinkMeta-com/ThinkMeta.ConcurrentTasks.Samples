// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

#include <CommCtrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdint>
#include <cwchar>

#include "Common.h"
#include "MainWindow.h"

// common controls 6 (themed controls)
#pragma comment( \
    linker,      \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace ThinkMeta::ConcurrentTasks::Samples::Opus::Gui;

namespace {

    // --autostart: start the live stream with the default settings (test signal, no playback)
    // --verify:    bit-compare every packet with libopus
    // --exclusive: WASAPI exclusive mode (falls back to shared)
    // --playback:  play to the default output device; --silent: write silence there (the whole path still runs)
    // --engine I:  the entry I of the encoder list (live: 0 ctOpus, 1 libopus; load: 0 to 3)
    // --load N:    start the multi-stream load with N streams
    // --seconds S: stop and close after S seconds
    // --report F:  write a summary to F on that close
    CommandLine ParseCommandLine() noexcept
    {
        CommandLine  result;
        std::int32_t argc = 0;
        wchar_t**    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv == nullptr)
            return result;
        for (std::int32_t i = 1; i < argc; ++i) {
            const wchar_t* arg  = argv[i];
            const wchar_t* next = i + 1 < argc ? argv[i + 1] : nullptr;
            if (wcscmp(arg, L"--engine") == 0 && next != nullptr) {
                result.engine = static_cast<std::int32_t>(wcstol(next, nullptr, 10));
                ++i;
            } else if (wcscmp(arg, L"--exclusive") == 0) {
                result.exclusive = true;
            } else if (wcscmp(arg, L"--playback") == 0) {
                result.playback = true;
            } else if (wcscmp(arg, L"--silent") == 0) {
                result.silent = true;
            } else if (wcscmp(arg, L"--verify") == 0) {
                result.verify = true;
            } else if (wcscmp(arg, L"--autostart") == 0) {
                result.autostart = true;
            } else if (wcscmp(arg, L"--load") == 0 && next != nullptr) {
                result.loadStreams = static_cast<std::int32_t>(wcstol(next, nullptr, 10));
                ++i;
            } else if (wcscmp(arg, L"--seconds") == 0 && next != nullptr) {
                result.seconds = static_cast<std::int32_t>(wcstol(next, nullptr, 10));
                ++i;
            } else if (wcscmp(arg, L"--report") == 0 && next != nullptr) {
                CopyText(result.report, MAX_PATH, next);
                ++i;
            }
        }
        LocalFree(argv);
        return result;
    }

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // the UI thread is an STA (common file dialog); the audio threads enter the MTA themselves
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return 1;
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);

    std::int32_t exitCode = 2;
    {
        MainWindow window;
        if (window.Create(instance, show, ParseCommandLine()))
            exitCode = window.Run();
    }
    CoUninitialize();
    return exitCode;
}
