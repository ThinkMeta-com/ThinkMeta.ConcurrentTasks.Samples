// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#include <Windows.h>

#include <CommCtrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>

#include "FrameCodec.h"
#include "MainWindow.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui {

    namespace {

        enum : std::int32_t {
            IdMode = 100,
            IdSource,
            IdBrowse,
            IdOutput,
            IdEngine,
            IdFrame,
            IdApplication,
            IdBitrate,
            IdComplexity,
            IdStreams,
            IdThreads,
            IdLowLatency,
            IdExclusive,
            IdMmcss,
            IdVerify,
            IdStartStop,
            IdStatus,
            IdWavName,
        };

        constexpr UINT_PTR TimerId     = 1;
        constexpr UINT     TimerPeriod = 50; // ms
        constexpr wchar_t  ClassName[] = L"ThinkMeta.ctOpus.Gui";

        // item data of the source and output lists
        constexpr LRESULT SourceTest    = 100001; // not CB_ERR (-1)
        constexpr LRESULT SourceWav     = 100002;
        constexpr LRESULT OutputNone    = 100003;
        constexpr LRESULT DefaultDevice = 0; // device k is k + 1

        constexpr std::int32_t FrameSizes[] = {120, 240, 480, 960};
        constexpr std::int32_t Bitrates[]   = {24, 32, 48, 64, 96, 128, 160, 192, 256};

        // colours of the visualisation
        constexpr COLORREF Background = RGB(22, 24, 29);
        constexpr COLORREF PanelColor = RGB(32, 35, 42);
        constexpr COLORREF GridColor  = RGB(52, 57, 67);
        constexpr COLORREF TextColor  = RGB(222, 226, 232);
        constexpr COLORREF DimColor   = RGB(140, 148, 160);
        constexpr COLORREF Blue       = RGB(80, 170, 255);
        constexpr COLORREF Orange     = RGB(255, 165, 60);
        constexpr COLORREF Green      = RGB(90, 200, 120);
        constexpr COLORREF Red        = RGB(235, 80, 80);
        constexpr COLORREF Yellow     = RGB(235, 210, 80);
        constexpr COLORREF Violet     = RGB(170, 130, 255);
        constexpr COLORREF Teal       = RGB(70, 200, 200);
        constexpr COLORREF Grey       = RGB(120, 128, 140);

        void Fill(HDC dc, const RECT& r, COLORREF color) noexcept
        {
            SetDCBrushColor(dc, color);
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }

        void Line(HDC dc, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, COLORREF color) noexcept
        {
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCPenColor(dc, color);
            MoveToEx(dc, x0, y0, nullptr);
            LineTo(dc, x1, y1);
        }

        void Polyline(HDC dc, const POINT* points, std::int32_t count, COLORREF color) noexcept
        {
            if (count < 2)
                return;
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCPenColor(dc, color);
            ::Polyline(dc, points, count);
        }

        void Text(HDC dc, std::int32_t x, std::int32_t y, COLORREF color, const wchar_t* format, ...) noexcept
        {
            wchar_t buffer[512];
            va_list args;
            va_start(args, format);
            const std::int32_t n = _vsnwprintf_s(buffer, 512, _TRUNCATE, format, args);
            va_end(args);
            if (n < 0)
                return;
            SetTextColor(dc, color);
            TextOutW(dc, x, y, buffer, n);
        }

        std::int32_t TextHeight(HDC dc) noexcept
        {
            TEXTMETRICW metrics;
            GetTextMetricsW(dc, &metrics);
            return metrics.tmHeight;
        }

        std::int32_t TextWidth(HDC dc, const wchar_t* text) noexcept
        {
            SIZE size;
            GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
            return size.cx;
        }

        // statistics of a history ring of count entries, newest at head - 1
        struct Stats {
            double average = 0;
            double maximum = 0;
            double p99     = 0;
        };

        Stats Summarise(const float* ring, std::int32_t count, std::int32_t head, std::int32_t capacity, std::int32_t last) noexcept
        {
            Stats s;
            if (count <= 0)
                return s;
            const std::int32_t n = last > 0 && last < count ? last : count;
            float              sorted[LiveSnapshot::HistorySize];
            double             sum = 0;
            for (std::int32_t i = 0; i < n; ++i) {
                const float v = ring[(head - 1 - i + capacity) % capacity];
                sorted[i]     = v;
                sum += v;
                if (v > s.maximum)
                    s.maximum = v;
            }
            s.average = sum / n;
            // the 99th percentile by partial selection: n is at most 512
            const std::int32_t k = n - 1 - n / 100;
            for (std::int32_t i = 0; i <= k; ++i) {
                std::int32_t best = i;
                for (std::int32_t j = i + 1; j < n; ++j) {
                    if (sorted[j] < sorted[best])
                        best = j;
                }
                const float t = sorted[i];
                sorted[i]     = sorted[best];
                sorted[best]  = t;
            }
            s.p99 = sorted[k];
            return s;
        }

        void AddItem(HWND combo, const wchar_t* text, LRESULT data) noexcept
        {
            const auto index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), data);
        }

        LRESULT SelectedData(HWND combo) noexcept
        {
            const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
            return index == CB_ERR ? CB_ERR : SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0);
        }

        const wchar_t* FileName(const wchar_t* path) noexcept
        {
            const wchar_t* name = path;
            for (const wchar_t* p = path; *p != 0; ++p) {
                if (*p == L'\\' || *p == L'/')
                    name = p + 1;
            }
            return name;
        }

    } // namespace

    MainWindow::MainWindow() noexcept = default;

    MainWindow::~MainWindow()
    {
        StopAll();
        if (uiFont_ != nullptr)
            DeleteObject(uiFont_);
        if (monoFont_ != nullptr)
            DeleteObject(monoFont_);
        if (titleFont_ != nullptr)
            DeleteObject(titleFont_);
    }

    bool MainWindow::Create(HINSTANCE instance, std::int32_t show, const CommandLine& commandLine) noexcept
    {
        instance_    = instance;
        commandLine_ = commandLine;
        testSignal_.MakeTestSignal();

        WNDCLASSEXW wc   = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WindowProc;
        wc.hInstance     = instance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = ClassName;
        if (RegisterClassExW(&wc) == 0)
            return false;

        hwnd_ = CreateWindowExW(0, ClassName, L"ctOpus Live - real-time Opus encoding on ConcurrentTasks", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr)
            return false;
        // the initial size for the monitor's DPI
        dpi_ = static_cast<std::int32_t>(GetDpiForWindow(hwnd_));
        SetWindowPos(hwnd_, nullptr, 0, 0, Scale(1280), Scale(820), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd_, show);
        UpdateWindow(hwnd_);

        startedAt_ = Now100ns();
        if (commandLine_.verify)
            SendMessageW(verify_, BM_SETCHECK, BST_CHECKED, 0);
        if (commandLine_.exclusive)
            SendMessageW(exclusive_, BM_SETCHECK, BST_CHECKED, 0);
        if (commandLine_.playback) {
            for (std::int32_t i = 0, n = static_cast<std::int32_t>(SendMessageW(output_, CB_GETCOUNT, 0, 0)); i < n; ++i) {
                if (SendMessageW(output_, CB_GETITEMDATA, static_cast<WPARAM>(i), 0) == DefaultDevice)
                    SendMessageW(output_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            }
            ShowDevicePeriods(L"Device periods (exclusive: IAudioClient::GetDevicePeriod; shared: IAudioClient3 engine periods):");
        }
        if (commandLine_.autostart || commandLine_.loadStreams > 0) {
            if (commandLine_.loadStreams > 0) {
                SendMessageW(mode_, CB_SETCURSEL, 1, 0);
                FillModeDependentLists();
                wchar_t text[16];
                swprintf_s(text, L"%d", commandLine_.loadStreams);
                SetWindowTextW(streams_, text);
            }
            SendMessageW(engine_, CB_SETCURSEL, static_cast<WPARAM>(commandLine_.engine), 0);
            UpdateControlState();
            StartStop();
        }
        return true;
    }

    std::int32_t MainWindow::Run() noexcept
    {
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return static_cast<std::int32_t>(message.wParam);
    }

    LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        MainWindow* self = nullptr;
        if (message == WM_NCCREATE) {
            self        = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self != nullptr ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        switch (message) {
            case WM_CREATE: OnCreate(); return 0;
            case WM_SIZE:
                Layout();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_GETMINMAXINFO: {
                auto* info             = reinterpret_cast<MINMAXINFO*>(lParam);
                info->ptMinTrackSize.x = Scale(980);
                info->ptMinTrackSize.y = Scale(700);
                return 0;
            }
            case WM_DPICHANGED: OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam)); return 0;
            case WM_COMMAND: OnCommand(LOWORD(wParam), HIWORD(wParam)); return 0;
            case WM_HSCROLL:
                if (reinterpret_cast<HWND>(lParam) == complexity_) {
                    UpdateComplexityLabel();
                    if (running_ && !runningLoad_)
                        live_.SetComplexity(static_cast<std::int32_t>(SendMessageW(complexity_, TBM_GETPOS, 0, 0)));
                }
                return 0;
            case WM_TIMER: OnTimer(); return 0;
            case WM_ERASEBKGND: {
                // the control pane only; the visualisation paints all of its area
                RECT client;
                GetClientRect(hwnd_, &client);
                RECT pane  = client;
                pane.right = VisualArea().left;
                FillRect(reinterpret_cast<HDC>(wParam), &pane, GetSysColorBrush(COLOR_BTNFACE));
                return 1;
            }
            case WM_PAINT: OnPaint(); return 0;
            case WM_CLOSE: DestroyWindow(hwnd_); return 0;
            case WM_DESTROY:
                KillTimer(hwnd_, TimerId);
                StopAll();
                PostQuitMessage(0);
                return 0;
            default: return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND MainWindow::AddControl(const wchar_t* className, const wchar_t* text, unsigned long style, std::int32_t id) noexcept
    {
        HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), FALSE);
        return control;
    }

    void MainWindow::CreateFonts() noexcept
    {
        if (uiFont_ != nullptr)
            DeleteObject(uiFont_);
        if (monoFont_ != nullptr)
            DeleteObject(monoFont_);
        if (titleFont_ != nullptr)
            DeleteObject(titleFont_);
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize            = sizeof(metrics);
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, static_cast<UINT>(dpi_));
        uiFont_       = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW mono = metrics.lfMessageFont;
        CopyText(mono.lfFaceName, LF_FACESIZE, L"Consolas");
        mono.lfHeight  = -Scale(13);
        monoFont_      = CreateFontIndirectW(&mono);
        LOGFONTW title = metrics.lfMessageFont;
        title.lfHeight = -Scale(16);
        title.lfWeight = FW_SEMIBOLD;
        titleFont_     = CreateFontIndirectW(&title);
    }

    void MainWindow::OnCreate() noexcept
    {
        dpi_ = static_cast<std::int32_t>(GetDpiForWindow(hwnd_));
        CreateFonts();

        modeLabel_ = AddControl(L"STATIC", L"Mode", 0, -1);
        mode_      = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdMode);
        SendMessageW(mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Live stream (capture -> encode -> playback)"));
        SendMessageW(mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Multi-stream load (deadline misses)"));
        SendMessageW(mode_, CB_SETCURSEL, 0, 0);

        sourceLabel_ = AddControl(L"STATIC", L"Source", 0, -1);
        source_      = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdSource);
        browse_      = AddControl(WC_BUTTONW, L"Open WAV...", BS_PUSHBUTTON | WS_TABSTOP, IdBrowse);
        wavName_     = AddControl(L"STATIC", L"(no WAV file loaded)", SS_ENDELLIPSIS | SS_CENTERIMAGE, IdWavName);

        outputLabel_ = AddControl(L"STATIC", L"Playback (decoded with libopus)", 0, -1);
        output_      = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdOutput);

        engineLabel_ = AddControl(L"STATIC", L"Encoder", 0, -1);
        engine_      = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdEngine);

        frameLabel_ = AddControl(L"STATIC", L"Frame size", 0, -1);
        frame_      = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdFrame);
        for (const wchar_t* text : {L"2.5 ms", L"5 ms", L"10 ms", L"20 ms"})
            SendMessageW(frame_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(frame_, CB_SETCURSEL, 1, 0);

        applicationLabel_ = AddControl(L"STATIC", L"Application", 0, -1);
        application_      = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IdApplication);
        for (const wchar_t* text : {L"Restricted low delay (CELT only)", L"Audio", L"VoIP"})
            SendMessageW(application_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(application_, CB_SETCURSEL, 0, 0);

        bitrateLabel_ = AddControl(L"STATIC", L"Bitrate (VBR)", 0, -1);
        bitrate_      = AddControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IdBitrate);
        for (const std::int32_t kbps : Bitrates) {
            wchar_t text[32];
            swprintf_s(text, L"%d kbit/s", kbps);
            SendMessageW(bitrate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        }
        SendMessageW(bitrate_, CB_SETCURSEL, 5, 0);

        complexityLabel_ = AddControl(L"STATIC", L"", 0, -1);
        complexity_      = AddControl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, IdComplexity);
        SendMessageW(complexity_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10));
        SendMessageW(complexity_, TBM_SETPOS, TRUE, 10);
        UpdateComplexityLabel();

        streamsLabel_ = AddControl(L"STATIC", L"Streams", 0, -1);
        streams_      = AddControl(WC_EDITW, L"64", ES_NUMBER | WS_BORDER | WS_TABSTOP, IdStreams);
        threadsLabel_ = AddControl(L"STATIC", L"Scheduler threads (0 = all processors)", 0, -1);
        threads_      = AddControl(WC_EDITW, L"0", ES_NUMBER | WS_BORDER | WS_TABSTOP, IdThreads);

        lowLatency_ = AddControl(WC_BUTTONW, L"Small WASAPI periods (IAudioClient3)", BS_AUTOCHECKBOX | WS_TABSTOP, IdLowLatency);
        mmcss_      = AddControl(WC_BUTTONW, L"MMCSS \"Pro Audio\" for audio and scheduler threads", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, IdMmcss);
        exclusive_
            = AddControl(WC_BUTTONW, L"WASAPI exclusive mode (lowest latency; falls back to shared)", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, IdExclusive);
        verify_ = AddControl(WC_BUTTONW, L"Bit-compare every packet with libopus", BS_AUTOCHECKBOX | WS_TABSTOP, IdVerify);
        SendMessageW(lowLatency_, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(mmcss_, BM_SETCHECK, BST_CHECKED, 0);

        startStop_ = AddControl(WC_BUTTONW, L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, IdStartStop);
        status_    = AddControl(L"STATIC", L"", SS_LEFT, IdStatus);

        RefreshDevices();
        FillModeDependentLists();
        wchar_t text[160];
        swprintf_s(text, L"Encoder under test: ctOpus (port of libopus 1.6.1). Comparison and decoder: %hs.", LibopusVersion());
        ShowDevicePeriods(text);
        SetTimer(hwnd_, TimerId, TimerPeriod, nullptr);
    }

    void MainWindow::OnDpiChanged(std::int32_t dpi, const RECT* suggested) noexcept
    {
        dpi_ = dpi;
        CreateFonts();
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), FALSE);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    RECT MainWindow::VisualArea() const noexcept
    {
        RECT client;
        GetClientRect(hwnd_, &client);
        client.left = Scale(320);
        return client;
    }

    void MainWindow::Layout() noexcept
    {
        if (status_ == nullptr)
            return;
        const bool         load  = LoadMode();
        const std::int32_t x     = Scale(14);
        const std::int32_t w     = Scale(292);
        const std::int32_t label = Scale(18);
        const std::int32_t combo = Scale(26);
        std::int32_t       y     = Scale(12);
        auto               place = [&](HWND control, std::int32_t height, bool visible, std::int32_t moveHeight = 0) {
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
            if (!visible)
                return;
            MoveWindow(control, x, y, w, moveHeight > 0 ? moveHeight : height, TRUE);
            y += height;
        };
        auto gap = [&](std::int32_t g) { y += Scale(g); };

        place(modeLabel_, label, true);
        place(mode_, combo, true, Scale(200));
        gap(6);
        place(sourceLabel_, label, true);
        place(source_, combo, true, Scale(300));
        gap(4);
        ShowWindow(browse_, SW_SHOW);
        ShowWindow(wavName_, SW_SHOW);
        MoveWindow(browse_, x, y, Scale(96), Scale(26), TRUE);
        MoveWindow(wavName_, x + Scale(104), y, w - Scale(104), Scale(26), TRUE);
        y += Scale(26);
        gap(6);
        place(outputLabel_, label, !load);
        place(output_, combo, !load, Scale(300));
        if (!load)
            gap(6);
        place(engineLabel_, label, true);
        place(engine_, combo, true, Scale(200));
        gap(6);
        place(frameLabel_, label, true);
        place(frame_, combo, true, Scale(200));
        gap(6);
        place(applicationLabel_, label, true);
        place(application_, combo, true, Scale(200));
        gap(6);
        place(bitrateLabel_, label, true);
        place(bitrate_, combo, true, Scale(300));
        gap(6);
        place(complexityLabel_, label, true);
        place(complexity_, Scale(30), true);
        gap(4);
        place(streamsLabel_, label, load);
        place(streams_, Scale(24), load);
        if (load)
            gap(6);
        place(threadsLabel_, label, load);
        place(threads_, Scale(24), load);
        if (load)
            gap(6);
        place(lowLatency_, Scale(24), !load);
        place(exclusive_, Scale(36), !load);
        place(mmcss_, Scale(36), true);
        place(verify_, Scale(24), !load);
        gap(10);
        place(startStop_, Scale(34), true);
        gap(10);
        RECT client;
        GetClientRect(hwnd_, &client);
        const std::int32_t statusHeight = client.bottom - y - Scale(10);
        place(status_, statusHeight > Scale(40) ? statusHeight : Scale(40), true);
    }

    bool MainWindow::LoadMode() const noexcept
    {
        return SendMessageW(mode_, CB_GETCURSEL, 0, 0) == 1;
    }

    std::int32_t MainWindow::ComboIndex(HWND combo) const noexcept
    {
        const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        return index == CB_ERR ? 0 : static_cast<std::int32_t>(index);
    }

    std::int32_t MainWindow::SelectedFrameSize() const noexcept
    {
        return FrameSizes[ComboIndex(frame_) & 3];
    }

    Application MainWindow::SelectedApplication() const noexcept
    {
        switch (ComboIndex(application_)) {
            case 1: return Application::Audio;
            case 2: return Application::Voip;
            default: return Application::RestrictedLowDelay;
        }
    }

    std::int32_t MainWindow::SelectedBitrate() const noexcept
    {
        const std::int32_t index = ComboIndex(bitrate_);
        return Bitrates[index < static_cast<std::int32_t>(sizeof(Bitrates) / sizeof(Bitrates[0])) ? index : 0] * 1000;
    }

    std::int32_t MainWindow::EditNumber(HWND edit, std::int32_t fallback) const noexcept
    {
        wchar_t text[32] = {};
        GetWindowTextW(edit, text, 32);
        wchar_t*   end   = nullptr;
        const long value = wcstol(text, &end, 10);
        return end != text ? static_cast<std::int32_t>(value) : fallback;
    }

    void MainWindow::RefreshDevices() noexcept
    {
        captureCount_ = EnumerateAudioDevices(true, captureDevices_, MaxDevices);
        renderCount_  = EnumerateAudioDevices(false, renderDevices_, MaxDevices);
    }

    void MainWindow::FillModeDependentLists() noexcept
    {
        const bool load = LoadMode();

        const LRESULT previousSource = SelectedData(source_);
        SendMessageW(source_, CB_RESETCONTENT, 0, 0);
        AddItem(source_, L"Test signal (synthetic, real-time)", SourceTest);
        AddItem(source_, L"WAV file (real-time, looped)", SourceWav);
        if (!load) {
            AddItem(source_, L"Default capture device", DefaultDevice);
            for (std::int32_t i = 0; i < captureCount_; ++i)
                AddItem(source_, captureDevices_[i].name, i + 1);
        }
        std::int32_t select = 0;
        for (std::int32_t i = 0, n = static_cast<std::int32_t>(SendMessageW(source_, CB_GETCOUNT, 0, 0)); i < n; ++i) {
            if (SendMessageW(source_, CB_GETITEMDATA, static_cast<WPARAM>(i), 0) == previousSource)
                select = i;
        }
        SendMessageW(source_, CB_SETCURSEL, static_cast<WPARAM>(select), 0);

        if (SendMessageW(output_, CB_GETCOUNT, 0, 0) == 0) {
            AddItem(output_, L"None (encode and decode only)", OutputNone);
            AddItem(output_, L"Default output device", DefaultDevice);
            for (std::int32_t i = 0; i < renderCount_; ++i)
                AddItem(output_, renderDevices_[i].name, i + 1);
            SendMessageW(output_, CB_SETCURSEL, 0, 0);
        }

        const std::int32_t previousEngine = ComboIndex(engine_);
        SendMessageW(engine_, CB_RESETCONTENT, 0, 0);
        if (load) {
            for (const wchar_t* text :
                 {L"ctOpus, one task per stream", L"ctOpus, one thread per stream", L"libopus, one thread per stream", L"libopus, one task per stream"})
                SendMessageW(engine_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        } else {
            SendMessageW(engine_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"ctOpus (encode task on the scheduler)"));
            SendMessageW(engine_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"libopus (encode task on the scheduler)"));
        }
        SendMessageW(engine_, CB_SETCURSEL, static_cast<WPARAM>(load ? previousEngine & 3 : previousEngine & 1), 0);
        UpdateControlState();
        Layout();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void MainWindow::UpdateControlState() noexcept
    {
        const bool idle = !running_;
        const bool load = LoadMode();
        for (HWND control : {mode_, source_, browse_, output_, engine_, frame_, application_, streams_, threads_, lowLatency_, exclusive_, mmcss_})
            EnableWindow(control, idle);
        // bitrate and complexity change a live stream on the fly
        EnableWindow(bitrate_, idle || !runningLoad_);
        EnableWindow(complexity_, idle || !runningLoad_);
        EnableWindow(verify_, idle && !load && ComboIndex(engine_) == 0);
        EnableWindow(threads_, idle && load && UsesTasks(static_cast<LoadEngine>(ComboIndex(engine_))));
        SetWindowTextW(startStop_, running_ ? L"Stop" : L"Start");
    }

    void MainWindow::UpdateComplexityLabel() noexcept
    {
        wchar_t text[64];
        swprintf_s(text, L"Complexity: %d", static_cast<std::int32_t>(SendMessageW(complexity_, TBM_GETPOS, 0, 0)));
        SetWindowTextW(complexityLabel_, text);
    }

    void MainWindow::SetStatus(const wchar_t* text) noexcept
    {
        SetWindowTextW(status_, text);
    }

    void MainWindow::OnCommand(std::int32_t id, std::int32_t code) noexcept
    {
        switch (id) {
            case IdMode:
                if (code == CBN_SELCHANGE)
                    FillModeDependentLists();
                break;
            case IdEngine:
                if (code == CBN_SELCHANGE)
                    UpdateControlState();
                break;
            case IdBitrate:
                if (code == CBN_SELCHANGE && running_ && !runningLoad_)
                    live_.SetBitrate(SelectedBitrate());
                break;
            case IdBrowse:
                if (code == BN_CLICKED)
                    BrowseWav();
                break;
            case IdSource:
            case IdOutput:
                if (code == CBN_SELCHANGE && !running_)
                    ShowDevicePeriods(L"Device periods (exclusive: IAudioClient::GetDevicePeriod; shared: IAudioClient3 engine periods):");
                break;
            case IdStartStop:
                if (code == BN_CLICKED)
                    StartStop();
                break;
            default: break;
        }
    }

    void MainWindow::BrowseWav() noexcept
    {
        wchar_t       path[MAX_PATH] = {};
        OPENFILENAMEW ofn            = {};
        ofn.lStructSize              = sizeof(ofn);
        ofn.hwndOwner                = hwnd_;
        ofn.lpstrFilter              = L"WAV files (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
        ofn.lpstrFile                = path;
        ofn.nMaxFile                 = MAX_PATH;
        ofn.Flags                    = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&ofn))
            return;
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        wchar_t error[160] = {};
        if (!wav_.LoadWav(path, error, 160)) {
            wavLoaded_ = false;
            SetWindowTextW(wavName_, L"(no WAV file loaded)");
            wchar_t text[400];
            swprintf_s(text, L"%ls: %ls", FileName(path), error);
            SetStatus(text);
            return;
        }
        wavLoaded_ = true;
        SetWindowTextW(wavName_, FileName(path));
        wchar_t text[400];
        swprintf_s(text, L"%ls: %d Hz, %d channel(s), %.1f s%ls (resampled to 48 kHz stereo where needed).", FileName(path), wav_.SourceRate(),
                   wav_.SourceChannels(), wav_.Seconds(), wav_.Seconds() >= SignalBuffer::MaxSeconds - 0.01 ? L" (cut)" : L"");
        SetStatus(text);
        for (std::int32_t i = 0, n = static_cast<std::int32_t>(SendMessageW(source_, CB_GETCOUNT, 0, 0)); i < n; ++i) {
            if (SendMessageW(source_, CB_GETITEMDATA, static_cast<WPARAM>(i), 0) == SourceWav)
                SendMessageW(source_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
        }
    }

    void MainWindow::StartStop() noexcept
    {
        if (running_) {
            StopAll();
            RefreshDevices();
            ShowDevicePeriods(L"Stopped.");
            UpdateControlState();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const bool load = LoadMode();
        const bool ok   = load ? StartLoad() : StartLive();
        if (ok) {
            running_     = true;
            runningLoad_ = load;
            startedAt_   = Now100ns();
            inPeak_ = outPeak_ = -100;
            loadHead_ = loadCount_ = 0;
        }
        UpdateControlState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool MainWindow::StartLive() noexcept
    {
        LiveConfig    config;
        const LRESULT source = SelectedData(source_);
        if (source == SourceWav && !wavLoaded_) {
            SetStatus(L"Load a WAV file first (Open WAV...).");
            return false;
        }
        config.source = source == SourceTest ? SourceKind::TestSignal : source == SourceWav ? SourceKind::WavFile : SourceKind::Device;
        config.signal = source == SourceWav ? &wav_ : &testSignal_;
        if (source > 0 && source <= captureCount_)
            CopyText(config.captureId, 256, captureDevices_[source - 1].id);
        const LRESULT output = SelectedData(output_);
        config.render        = output != OutputNone && output != CB_ERR;
        if (output > 0 && output <= renderCount_)
            CopyText(config.renderId, 256, renderDevices_[output - 1].id);
        config.engine       = ComboIndex(engine_) == 0 ? EncoderKind::CtOpus : EncoderKind::Libopus;
        config.verify       = config.engine == EncoderKind::CtOpus && SendMessageW(verify_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        config.frameSize    = SelectedFrameSize();
        config.application  = SelectedApplication();
        config.bitrate      = SelectedBitrate();
        config.complexity   = static_cast<std::int32_t>(SendMessageW(complexity_, TBM_GETPOS, 0, 0));
        config.lowLatency   = SendMessageW(lowLatency_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        config.exclusive    = SendMessageW(exclusive_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        config.silentOutput = commandLine_.silent;
        config.mmcss        = SendMessageW(mmcss_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        wchar_t error[256]  = {};
        if (!live_.Start(config, error, 256)) {
            SetStatus(error);
            return false;
        }
        // what actually runs (exclusive mode may have fallen back to shared)
        live_.Snapshot(liveSnap_);
        wchar_t capture[256]  = L"timer-paced file/test signal, 48000 Hz";
        wchar_t playback[256] = L"none";
        if (!liveSnap_.captureSimulated)
            DescribeSetup(liveSnap_.capture, capture, 256);
        if (liveSnap_.rendering)
            DescribeSetup(liveSnap_.render, playback, 256);
        wchar_t capturePeriods[320]  = L"Capture: timer 2.50 ms (file/test signal)";
        wchar_t playbackPeriods[320] = L"Playback: none";
        if (!liveSnap_.captureSimulated)
            DescribePeriods(L"Capture", liveSnap_.capture.periods, &liveSnap_.capture, capturePeriods, 320);
        if (liveSnap_.rendering)
            DescribePeriods(L"Playback", liveSnap_.render.periods, &liveSnap_.render, playbackPeriods, 320);
        wchar_t text[1400];
        swprintf_s(text, L"Running. Bitrate and complexity can be changed while running.\r\nCapture: %ls.\r\n%ls\r\nPlayback: %ls.\r\n%ls", capture,
                   capturePeriods, playback, playbackPeriods);
        SetStatus(text);
        return true;
    }

    void MainWindow::ShowDevicePeriods(const wchar_t* first) noexcept
    {
        // what the selected devices offer, queried without opening a stream
        wchar_t       capture[320]  = L"Capture: timer 2.50 ms (file/test signal)";
        wchar_t       playback[320] = L"Playback: none";
        const LRESULT source        = SelectedData(source_);
        if (!LoadMode() && source != SourceTest && source != SourceWav && source != CB_ERR) {
            DevicePeriods periods;
            QueryDevicePeriods(true, source > 0 && source <= captureCount_ ? captureDevices_[source - 1].id : nullptr, periods);
            DescribePeriods(L"Capture", periods, nullptr, capture, 320);
        }
        const LRESULT output = SelectedData(output_);
        if (!LoadMode() && output != OutputNone && output != CB_ERR) {
            DevicePeriods periods;
            QueryDevicePeriods(false, output > 0 && output <= renderCount_ ? renderDevices_[output - 1].id : nullptr, periods);
            DescribePeriods(L"Playback", periods, nullptr, playback, 320);
        }
        wchar_t text[1200];
        swprintf_s(text, L"%ls\r\n%ls\r\n%ls", first, capture, playback);
        SetStatus(text);
    }

    bool MainWindow::StartLoad() noexcept
    {
        LoadConfig    config;
        const LRESULT source = SelectedData(source_);
        if (source == SourceWav && !wavLoaded_) {
            SetStatus(L"Load a WAV file first (Open WAV...).");
            return false;
        }
        config.signal        = source == SourceWav ? &wav_ : &testSignal_;
        config.engine        = static_cast<LoadEngine>(ComboIndex(engine_) & 3);
        config.streams       = EditNumber(streams_, 64);
        config.workerThreads = static_cast<std::uint32_t>(EditNumber(threads_, 0) > 0 ? EditNumber(threads_, 0) : 0);
        config.frameSize     = SelectedFrameSize();
        config.application   = SelectedApplication();
        config.bitrate       = SelectedBitrate();
        config.complexity    = static_cast<std::int32_t>(SendMessageW(complexity_, TBM_GETPOS, 0, 0));
        config.mmcss         = SendMessageW(mmcss_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (config.streams < 1 || config.streams > 4096) {
            SetStatus(L"Streams: 1 to 4096.");
            return false;
        }
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        wchar_t error[256] = {};
        if (!load_.Start(config, error, 256)) {
            SetStatus(error);
            return false;
        }
        SetStatus(L"Running. Every stream has its own encoder and its own position in the signal; a clock thread makes a frame of every stream "
                  L"due each frame length. A miss: the packet was ready more than one frame length after its frame was due.");
        return true;
    }

    void MainWindow::StopAll() noexcept
    {
        live_.Stop();
        load_.Stop();
        running_     = false;
        runningLoad_ = false;
    }

    void MainWindow::OnTimer() noexcept
    {
        if (running_ && runningLoad_) {
            load_.Poll(loadSnap_);
            loadMax_[loadHead_]    = static_cast<float>(loadSnap_.windowMaxMs);
            loadMisses_[loadHead_] = static_cast<float>(loadSnap_.windowMisses);
            loadHead_              = (loadHead_ + 1) % LoadHistory;
            if (loadCount_ < LoadHistory)
                ++loadCount_;
        } else if (running_) {
            live_.Snapshot(liveSnap_);
            const float decay = 20.0f * TimerPeriod / 1000.0f; // 20 dB/s
            inPeak_           = liveSnap_.inputDb > inPeak_ - decay ? liveSnap_.inputDb : inPeak_ - decay;
            outPeak_          = liveSnap_.outputDb > outPeak_ - decay ? liveSnap_.outputDb : outPeak_ - decay;
        }
        if (running_) {
            const RECT area = VisualArea();
            InvalidateRect(hwnd_, &area, FALSE);
        }

        // unattended runs: close after the given time (also when the start failed)
        if (commandLine_.seconds > 0 && Now100ns() - startedAt_ >= static_cast<std::int64_t>(commandLine_.seconds) * 10'000'000) {
            WriteReport();
            commandLine_.seconds = 0;
            DestroyWindow(hwnd_);
        }
    }

    void MainWindow::WriteReport() noexcept
    {
        if (commandLine_.report[0] == 0)
            return;
        char               text[4096];
        std::int32_t       n    = 0;
        const std::int32_t size = sizeof(text);
        if (!running_) {
            wchar_t status[512] = {};
            GetWindowTextW(status_, status, 512);
            n = snprintf(text, size, "mode=none\r\nstatus=%ls\r\n", status);
        } else if (runningLoad_) {
            load_.Poll(loadSnap_);
            const LoadSnapshot& s = loadSnap_;
            n                     = snprintf(
                text, size,
                "mode=load\r\nengine=%ls\r\nstreams=%d\r\nthreads=%d\r\nframe_ms=%.2f\r\nseconds=%.2f\r\nframes=%lld\r\nmisses=%lld\r\ndropped=%lld\r\n"
                "encode_us=%.1f\r\nlatency_ms=%.3f\r\np99_ms=%.3f\r\nmax_ms=%.3f\r\npacer_late_ms=%.3f\r\ncpu_percent=%.1f\r\n",
                s.engineName, s.streams, s.osThreads, s.frameMs, s.seconds, s.frames, s.misses, s.dropped, s.encodeUs, s.latencyMs, s.p99Ms, s.maxMs,
                s.pacerLateMs, s.cpuPercent);
        } else {
            live_.Snapshot(liveSnap_);
            const LiveSnapshot& s                = liveSnap_;
            const Stats         encode           = Summarise(s.encodeUs, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, 0);
            const Stats         ready            = Summarise(s.readyMs, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, 0);
            const Stats         decode           = Summarise(s.decodeUs, s.decodeCount, s.decodeHead, LiveSnapshot::HistorySize, 0);
            wchar_t             captureText[256] = L"timer (file/test signal)";
            wchar_t             renderText[256]  = L"none";
            wchar_t             statusLine[1400] = {};
            GetWindowTextW(status_, statusLine, 1400);
            if (!s.captureSimulated)
                DescribeSetup(s.capture, captureText, 256);
            if (s.rendering)
                DescribeSetup(s.render, renderText, 256);
            n = snprintf(text, size,
                         "mode=live\r\nengine=%ls\r\nframe_ms=%.2f\r\nlookahead_ms=%.2f\r\ncapture_period_ms=%.2f\r\nframes=%lld\r\nmisses=%lld\r\n"
                         "overruns=%lld\r\nverified=%lld\r\nmismatches=%lld\r\nencode_us=%.1f\r\nencode_max_us=%.1f\r\nready_ms=%.3f\r\ndecode_us=%.1f\r\n"
                         "capture=%ls\r\nplayback=%ls\r\nunderruns=%lld\r\nemergency_cuts=%lld\r\ndrift_ppm=%.1f\r\nqueue_ms=%.3f\r\ntarget_ms=%.3f\r\n"
                         "resample_ms=%.3f\r\nstatus=%ls\r\nstatus_line=%ls\r\n",
                         s.engineName, s.frameMs, s.lookaheadMs, s.capture.PeriodMs(), s.frames, s.misses, s.inputOverruns, s.verified, s.mismatches,
                         encode.average, encode.maximum, ready.average, decode.average, captureText, renderText, s.underruns, s.driftSkips, s.driftPpm,
                         s.fillMs, s.jitterBufferMs, s.captureResampleMs + s.renderResampleMs, s.status, statusLine);
        }
        if (n <= 0)
            return;
        HANDLE file = CreateFileW(commandLine_.report, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;
        unsigned long written = 0; // DWORD of WriteFile
        WriteFile(file, text, static_cast<unsigned long>(n < size ? n : size - 1), &written, nullptr);
        CloseHandle(file);
    }

    // --- drawing -------------------------------------------------------------------------------------

    void MainWindow::OnPaint() noexcept
    {
        PAINTSTRUCT ps;
        HDC         dc   = BeginPaint(hwnd_, &ps);
        const RECT  area = VisualArea();
        const auto  w    = area.right - area.left;
        const auto  h    = area.bottom - area.top;
        if (w > 0 && h > 0) {
            HDC     memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, w, h);
            HGDIOBJ old    = SelectObject(memory, bitmap);
            SetBkMode(memory, TRANSPARENT);
            const RECT local = {0, 0, w, h};
            Fill(memory, local, Background);
            if (running_ && runningLoad_)
                DrawLoad(memory, local);
            else if (running_)
                DrawLive(memory, local);
            else
                DrawIdle(memory, local);
            BitBlt(dc, area.left, area.top, w, h, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old);
            DeleteObject(bitmap);
            DeleteDC(memory);
        }
        EndPaint(hwnd_, &ps);
    }

    void MainWindow::DrawPanel(HDC dc, const RECT& r, const wchar_t* title) noexcept
    {
        Fill(dc, r, PanelColor);
        SelectObject(dc, uiFont_);
        Text(dc, r.left + Scale(8), r.top + Scale(4), DimColor, L"%ls", title);
    }

    void MainWindow::DrawIdle(HDC dc, const RECT& area) noexcept
    {
        const std::int32_t x = area.left + Scale(28);
        std::int32_t       y = area.top + Scale(28);
        SelectObject(dc, titleFont_);
        Text(dc, x, y, TextColor, L"ctOpus Live");
        y += Scale(34);
        SelectObject(dc, uiFont_);
        const std::int32_t line    = TextHeight(dc) + Scale(4);
        const wchar_t*     lines[] = {
            L"Live stream: capture thread (WASAPI, event-driven) -> ring buffer -> encode task on the ConcurrentTasks scheduler",
            L"-> monitor task (libopus decoder) -> playback queue -> LiveResampler (drift control) -> render thread (WASAPI, event-driven).",
            L"WASAPI shared (IAudioClient3, smallest period) or exclusive mode; devices not at 48 kHz are resampled with the LiveResampler.",
            L"Latency is shown as it is: frame length, codec lookahead, capture-to-encode, encode, decode and playback queue.",
            L"",
            L"Multi-stream load: many live streams at once, each with its own encoder; a frame of every stream is due every frame length.",
            L"Deadline misses are counted: ctOpus or libopus, one task per stream or one OS thread per stream.",
            L"",
            L"A WAV file or the test signal is played in real time (timer-paced), so all modes behave like live input.",
        };
        for (const wchar_t* text : lines) {
            Text(dc, x, y, text[0] == 0 ? DimColor : TextColor, L"%ls", text);
            y += line;
        }
    }

    void MainWindow::DrawLive(HDC dc, const RECT& area) noexcept
    {
        const LiveSnapshot& s   = liveSnap_;
        const std::int32_t  pad = Scale(12);
        RECT                r   = {area.left + pad, area.top + pad, area.right - pad, area.top + pad + Scale(26)};
        SelectObject(dc, titleFont_);
        Text(dc, r.left, r.top, TextColor, L"%ls  |  %.1f ms frames  |  %d kbit/s  |  complexity %d", s.engineName, s.frameMs, SelectedBitrate() / 1000,
             static_cast<std::int32_t>(SendMessageW(complexity_, TBM_GETPOS, 0, 0)));
        const std::int32_t height = area.bottom - area.top - 2 * pad - Scale(30);
        std::int32_t       y      = r.bottom + Scale(4);

        RECT wave = {r.left, y, r.right, y + height * 20 / 100};
        DrawWaveform(dc, wave);
        y = wave.bottom + Scale(8);

        const std::int32_t meterHeight = Scale(22);
        RECT               inMeter     = {r.left, y, (r.left + r.right) / 2 - Scale(6), y + meterHeight};
        RECT               outMeter    = {(r.left + r.right) / 2 + Scale(6), y, r.right, y + meterHeight};
        DrawMeter(dc, inMeter, L"Input", s.inputDb, inPeak_);
        DrawMeter(dc, outMeter, L"Decoded", s.outputDb, outPeak_);
        y = inMeter.bottom + Scale(8);

        RECT graph = {r.left, y, r.right, y + height * 30 / 100};
        DrawLatencyGraph(dc, graph);
        y = graph.bottom + Scale(8);

        RECT budget = {r.left, y, r.right, y + Scale(100)};
        DrawBudget(dc, budget);
        y = budget.bottom + Scale(8);

        RECT metrics = {r.left, y, r.right, area.bottom - pad};
        DrawLiveMetrics(dc, metrics);
    }

    void MainWindow::DrawWaveform(HDC dc, const RECT& r) noexcept
    {
        DrawPanel(dc, r, L"Input waveform (last ~170 ms)");
        const LiveSnapshot& s     = liveSnap_;
        const std::int32_t  mid   = (r.top + r.bottom) / 2;
        const std::int32_t  half  = (r.bottom - r.top) / 2 - Scale(6);
        const std::int32_t  width = r.right - r.left - Scale(16);
        Line(dc, r.left + Scale(8), mid, r.right - Scale(8), mid, GridColor);
        POINT points[LiveSnapshot::WaveformSize];
        for (std::int32_t i = 0; i < LiveSnapshot::WaveformSize; ++i) {
            float v     = s.waveform[(s.waveformHead + i) % LiveSnapshot::WaveformSize];
            v           = v > 1 ? 1 : v < -1 ? -1 : v;
            points[i].x = r.left + Scale(8) + i * width / (LiveSnapshot::WaveformSize - 1);
            points[i].y = mid - static_cast<std::int32_t>(v * static_cast<float>(half));
        }
        Polyline(dc, points, LiveSnapshot::WaveformSize, Teal);
    }

    void MainWindow::DrawMeter(HDC dc, const RECT& r, const wchar_t* label, float db, float peak) noexcept
    {
        Fill(dc, r, PanelColor);
        SelectObject(dc, uiFont_);
        const std::int32_t labelWidth = Scale(64);
        Text(dc, r.left + Scale(6), r.top + (r.bottom - r.top - TextHeight(dc)) / 2, DimColor, L"%ls", label);
        RECT bar = {r.left + labelWidth, r.top + Scale(5), r.right - Scale(70), r.bottom - Scale(5)};
        Fill(dc, bar, Background);
        auto position = [&](float value) {
            const float clamped = value < -60 ? -60 : value > 0 ? 0 : value;
            return bar.left + static_cast<std::int32_t>((clamped + 60) / 60 * static_cast<float>(bar.right - bar.left));
        };
        RECT level  = bar;
        level.right = position(db);
        Fill(dc, level, db > -3 ? Red : db > -12 ? Yellow : Green);
        const std::int32_t p = position(peak);
        Line(dc, p, bar.top, p, bar.bottom, TextColor);
        Text(dc, bar.right + Scale(8), r.top + (r.bottom - r.top - TextHeight(dc)) / 2, TextColor, db <= -99 ? L"  -inf dB" : L"%6.1f dB", db);
    }

    void MainWindow::DrawLatencyGraph(HDC dc, const RECT& r) noexcept
    {
        DrawPanel(dc, r, L"Per frame: newest sample's age when the packet is ready (blue), encode time (orange), frame length (grey)");
        const LiveSnapshot& s     = liveSnap_;
        const RECT          plot  = {r.left + Scale(48), r.top + Scale(34), r.right - Scale(10), r.bottom - Scale(8)};
        const std::int32_t  n     = s.encodeCount;
        double              scale = 2 * s.frameMs;
        for (std::int32_t i = 0; i < n; ++i) {
            const float v = s.readyMs[(s.encodeHead - 1 - i + LiveSnapshot::HistorySize) % LiveSnapshot::HistorySize];
            if (v * 1.1 > scale)
                scale = v * 1.1;
        }
        if (scale <= 0)
            scale = 1;
        SelectObject(dc, uiFont_);
        for (std::int32_t g = 0; g <= 4; ++g) {
            const std::int32_t y = plot.bottom - g * (plot.bottom - plot.top) / 4;
            Line(dc, plot.left, y, plot.right, y, GridColor);
            Text(dc, r.left + Scale(6), y - TextHeight(dc) / 2, DimColor, L"%.1f", scale * g / 4);
        }
        auto yOf = [&](double ms) {
            const double clamped = ms < 0 ? 0 : ms > scale ? scale : ms;
            return plot.bottom - static_cast<std::int32_t>(clamped / scale * (plot.bottom - plot.top));
        };
        const std::int32_t frameY = yOf(s.frameMs);
        Line(dc, plot.left, frameY, plot.right, frameY, Grey);
        if (n < 2)
            return;
        POINT              ready[LiveSnapshot::HistorySize];
        POINT              encode[LiveSnapshot::HistorySize];
        const std::int32_t width = plot.right - plot.left;
        for (std::int32_t i = 0; i < n; ++i) {
            const std::int32_t at = (s.encodeHead - n + i + LiveSnapshot::HistorySize) % LiveSnapshot::HistorySize;
            const std::int32_t x  = plot.left + (LiveSnapshot::HistorySize - n + i) * width / (LiveSnapshot::HistorySize - 1);
            ready[i]              = {x, yOf(s.readyMs[at])};
            encode[i]             = {x, yOf(s.encodeUs[at] / 1000.0)};
        }
        Polyline(dc, encode, n, Orange);
        Polyline(dc, ready, n, Blue);
    }

    void MainWindow::DrawBudget(HDC dc, const RECT& r) noexcept
    {
        DrawPanel(dc, r, L"Latency of the first sample of a frame, capture to playback (averages of the last 512 frames)");
        const LiveSnapshot& s      = liveSnap_;
        const Stats         age    = Summarise(s.ageMs, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, 0);
        const Stats         encode = Summarise(s.encodeUs, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, 0);
        const Stats         decode = Summarise(s.decodeUs, s.decodeCount, s.decodeHead, LiveSnapshot::HistorySize, 0);
        const Stats         queue  = Summarise(s.renderQueueMs, s.decodeCount, s.decodeHead, LiveSnapshot::HistorySize, 0);
        struct Part {
            const wchar_t* name;
            double         ms;
            COLORREF       color;
        };
        const Part parts[] = {
            {L"frame", s.frameMs, Grey},
            {L"lookahead", s.lookaheadMs, Violet},
            {L"capture->encode", age.average, Teal},
            {L"encode", encode.average / 1000.0, Orange},
            {L"decode", decode.average / 1000.0, Yellow},
            {L"resampling", s.captureResampleMs + (s.rendering ? s.renderResampleMs : 0.0), Red},
            {L"playback queue", s.rendering ? queue.average : 0.0, Blue},
            {L"render device", s.rendering ? s.renderStreamLatency : 0.0, Green},
        };
        double total = 0;
        for (const Part& p : parts)
            total += p.ms;
        if (total <= 0)
            return;
        const RECT bar   = {r.left + Scale(8), r.top + Scale(26), r.right - Scale(8), r.top + Scale(46)};
        double     start = 0;
        for (const Part& p : parts) {
            RECT segment  = bar;
            segment.left  = bar.left + static_cast<std::int32_t>(start / total * (bar.right - bar.left));
            segment.right = bar.left + static_cast<std::int32_t>((start + p.ms) / total * (bar.right - bar.left));
            Fill(dc, segment, p.color);
            start += p.ms;
        }
        SelectObject(dc, uiFont_);
        std::int32_t       x    = r.left + Scale(8);
        std::int32_t       y    = bar.bottom + Scale(8);
        const std::int32_t line = TextHeight(dc) + Scale(2);
        for (const Part& p : parts) {
            wchar_t text[64];
            swprintf_s(text, L"%ls %.2f", p.name, p.ms);
            const std::int32_t width = Scale(14) + TextWidth(dc, text) + Scale(14);
            if (x + width > r.right - Scale(8)) {
                x = r.left + Scale(8);
                y += line;
            }
            RECT swatch = {x, y + Scale(4), x + Scale(10), y + Scale(14)};
            Fill(dc, swatch, p.color);
            Text(dc, x + Scale(14), y, TextColor, L"%ls", text);
            x += width;
        }
        wchar_t sum[64];
        swprintf_s(sum, L"= %.1f ms%ls", total, s.rendering ? L"" : L" (no playback)");
        if (x + TextWidth(dc, sum) > r.right - Scale(8)) {
            x = r.left + Scale(8);
            y += line;
        }
        Text(dc, x, y, TextColor, L"%ls", sum);
    }

    void MainWindow::DrawLiveMetrics(HDC dc, const RECT& r) noexcept
    {
        Fill(dc, r, PanelColor);
        const LiveSnapshot& s         = liveSnap_;
        const Stats         encode    = Summarise(s.encodeUs, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, 0);
        const Stats         reference = Summarise(s.referenceUs, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, 0);
        const Stats         decode    = Summarise(s.decodeUs, s.decodeCount, s.decodeHead, LiveSnapshot::HistorySize, 0);
        const Stats         ready     = Summarise(s.readyMs, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, 0);
        const std::int32_t  perSecond = s.frameMs > 0 ? static_cast<std::int32_t>(1000.0 / s.frameMs) : 1;
        const Stats         bytes     = Summarise(s.packetBytes, s.encodeCount, s.encodeHead, LiveSnapshot::HistorySize, perSecond);
        const double        kbps      = s.frameMs > 0 ? bytes.average * 8 / s.frameMs : 0;

        SelectObject(dc, monoFont_);
        const std::int32_t line    = TextHeight(dc) + Scale(2);
        std::int32_t       x       = r.left + Scale(10);
        std::int32_t       y       = r.top + Scale(8);
        const double       missPct = s.frames > 0 ? 100.0 * static_cast<double>(s.misses) / static_cast<double>(s.frames) : 0;
        Text(dc, x, y, TextColor, L"Frames encoded     %lld", s.frames);
        y += line;
        Text(dc, x, y, s.misses > 0 ? Red : TextColor, L"Deadline misses    %lld (%.2f %%)", s.misses, missPct);
        y += line;
        Text(dc, x, y, DimColor, L"  (packet later than delivery of its input + one frame)");
        y += line;
        Text(dc, x, y, s.inputOverruns > 0 ? Yellow : TextColor, L"Input overruns     %lld", s.inputOverruns);
        y += line;
        Text(dc, x, y, s.discontinuities > 0 ? Yellow : TextColor, L"Capture glitches   %lld", s.discontinuities);
        y += line;
        if (s.rendering) {
            Text(dc, x, y, s.underruns > 0 ? Yellow : TextColor, L"Render underruns   %lld  (emergency cuts %lld)", s.underruns, s.driftSkips);
            y += line;
        }
        if (s.verified > 0) {
            Text(dc, x, y, s.mismatches > 0 ? Red : Green, L"Bit-compare        %lld of %lld packets differ", s.mismatches, s.verified);
            y += line;
        }

        const std::int32_t leftBottom = y;
        x                             = (r.left + r.right) / 2;
        y                             = r.top + Scale(8);
        Text(dc, x, y, TextColor, L"Encode    avg %6.1f us  p99 %6.1f  max %6.1f", encode.average, encode.p99, encode.maximum);
        y += line;
        if (s.verified > 0) {
            Text(dc, x, y, TextColor, L"libopus   avg %6.1f us  p99 %6.1f  (same frame)", reference.average, reference.p99);
            y += line;
        }
        Text(dc, x, y, TextColor, L"Decode    avg %6.1f us  (libopus, monitor task)", decode.average);
        y += line;
        Text(dc, x, y, TextColor, L"Ready     avg %6.2f ms  p99 %6.2f  max %6.2f", ready.average, ready.p99, ready.maximum);
        y += line;
        Text(dc, x, y, TextColor, L"Bitrate   %.1f kbit/s (last second)", kbps);
        y += line;
        if (s.rendering) {
            const double ppm = s.driftPpm;
            Text(dc, x, y, ppm > 4000 || ppm < -4000 ? Yellow : TextColor, L"Drift     %+.1f ppm  queue %.2f ms, target %.2f ms", ppm, s.fillMs,
                 s.jitterBufferMs);
            y += line;
        }

        // what actually runs, over the full width
        x = r.left + Scale(10);
        y = (y > leftBottom ? y : leftBottom) + line / 2;
        wchar_t text[256];
        if (s.captureSimulated) {
            Text(dc, x, y, TextColor, L"Capture   %.2f ms timer period, 48000 Hz (file/test signal)", s.capture.PeriodMs());
        } else {
            DescribeSetup(s.capture, text, 256);
            Text(dc, x, y, s.capture.exclusiveRequested && !s.capture.exclusive ? Yellow : TextColor, L"Capture   %ls%ls", text,
                 s.captureResampleMs > 0 ? L", resampled to 48 kHz" : L"");
        }
        y += line;
        if (s.rendering) {
            DescribeSetup(s.render, text, 256);
            Text(dc, x, y, s.render.exclusiveRequested && !s.render.exclusive ? Yellow : TextColor, L"Playback  %ls, buffer %.2f ms, stream latency %.2f ms",
                 text, s.render.BufferMs(), s.renderStreamLatency);
            y += line;
        }
        if (s.status[0] != 0 && wcscmp(s.status, L"Running.") != 0)
            Text(dc, x, y, Red, L"%ls", s.status);
    }

    void MainWindow::DrawLoad(HDC dc, const RECT& area) noexcept
    {
        const LoadSnapshot& s   = loadSnap_;
        const std::int32_t  pad = Scale(12);
        RECT                r   = {area.left + pad, area.top + pad, area.right - pad, area.top + pad + Scale(26)};
        SelectObject(dc, titleFont_);
        Text(dc, r.left, r.top, TextColor, L"%ls  |  %d streams on %d OS threads  |  %.1f ms frames  |  %d kbit/s  |  complexity %d", s.engineName, s.streams,
             s.osThreads, s.frameMs, SelectedBitrate() / 1000, static_cast<std::int32_t>(SendMessageW(complexity_, TBM_GETPOS, 0, 0)));
        const std::int32_t height = area.bottom - area.top - 2 * pad - Scale(30);
        std::int32_t       y      = r.bottom + Scale(4);
        RECT               graph  = {r.left, y, r.right, y + height * 38 / 100};
        DrawLoadGraph(dc, graph);
        y              = graph.bottom + Scale(8);
        RECT histogram = {r.left, y, r.right, y + height * 30 / 100};
        DrawHistogram(dc, histogram);
        y            = histogram.bottom + Scale(8);
        RECT metrics = {r.left, y, r.right, area.bottom - pad};
        DrawLoadMetrics(dc, metrics);
    }

    void MainWindow::DrawLoadGraph(HDC dc, const RECT& r) noexcept
    {
        DrawPanel(dc, r, L"Worst lateness of any stream per 50 ms (blue), deadline = one frame (red line), misses per 50 ms (red bars)");
        const LoadSnapshot& s           = loadSnap_;
        const RECT          plot        = {r.left + Scale(48), r.top + Scale(34), r.right - Scale(10), r.bottom - Scale(8)};
        double              top         = 2 * s.frameMs;
        float               worstMisses = 1;
        for (std::int32_t i = 0; i < loadCount_; ++i) {
            const std::int32_t at = (loadHead_ - 1 - i + LoadHistory) % LoadHistory;
            if (loadMax_[at] * 1.1 > top)
                top = loadMax_[at] * 1.1;
            if (loadMisses_[at] > worstMisses)
                worstMisses = loadMisses_[at];
        }
        if (top <= 0)
            top = 1;
        SelectObject(dc, uiFont_);
        for (std::int32_t g = 0; g <= 4; ++g) {
            const std::int32_t y = plot.bottom - g * (plot.bottom - plot.top) / 4;
            Line(dc, plot.left, y, plot.right, y, GridColor);
            Text(dc, r.left + Scale(6), y - TextHeight(dc) / 2, DimColor, L"%.1f", top * g / 4);
        }
        auto yOf = [&](double ms) {
            const double clamped = ms < 0 ? 0 : ms > top ? top : ms;
            return plot.bottom - static_cast<std::int32_t>(clamped / top * (plot.bottom - plot.top));
        };
        const std::int32_t width = plot.right - plot.left;
        POINT              points[LoadHistory];
        for (std::int32_t i = 0; i < loadCount_; ++i) {
            const std::int32_t at = (loadHead_ - loadCount_ + i + LoadHistory) % LoadHistory;
            const std::int32_t x  = plot.left + (LoadHistory - loadCount_ + i) * width / (LoadHistory - 1);
            if (loadMisses_[at] > 0) {
                const auto barHeight = static_cast<std::int32_t>(loadMisses_[at] / worstMisses * (plot.bottom - plot.top) / 3);
                RECT       bar       = {x - 1, plot.bottom - barHeight, x + 2, plot.bottom};
                Fill(dc, bar, Red);
            }
            points[i] = {x, yOf(loadMax_[at])};
        }
        const std::int32_t deadline = yOf(s.frameMs);
        Line(dc, plot.left, deadline, plot.right, deadline, Red);
        Polyline(dc, points, loadCount_, Blue);
    }

    void MainWindow::DrawHistogram(HDC dc, const RECT& r) noexcept
    {
        DrawPanel(dc, r, L"Lateness of all packets (time from the frame being due to its packet), log scale; red: deadline missed");
        const LoadSnapshot& s = loadSnap_;
        if (s.binMs <= 0)
            return;
        const RECT   plot    = {r.left + Scale(48), r.top + Scale(30), r.right - Scale(10), r.bottom - Scale(22)};
        std::int64_t largest = 1;
        for (const std::int64_t count : s.histogram)
            largest = count > largest ? count : largest;
        const double       logMax = std::log10(static_cast<double>(largest) + 1);
        const std::int32_t width  = plot.right - plot.left;
        for (std::int32_t b = 0; b < LoadSnapshot::Bins; ++b) {
            if (s.histogram[b] == 0)
                continue;
            const double fraction = std::log10(static_cast<double>(s.histogram[b]) + 1) / logMax;
            RECT         bar = {plot.left + b * width / LoadSnapshot::Bins + 1, plot.bottom - static_cast<std::int32_t>(fraction * (plot.bottom - plot.top)),
                                plot.left + (b + 1) * width / LoadSnapshot::Bins - 1, plot.bottom};
            Fill(dc, bar, b * s.binMs >= s.frameMs ? Red : Blue);
        }
        SelectObject(dc, uiFont_);
        Line(dc, plot.left, plot.bottom, plot.right, plot.bottom, GridColor);
        Text(dc, plot.left, plot.bottom + Scale(3), DimColor, L"0 ms");
        const std::int32_t deadlineX = plot.left + static_cast<std::int32_t>(s.frameMs / s.binMs) * width / LoadSnapshot::Bins;
        Line(dc, deadlineX, plot.top, deadlineX, plot.bottom, Red);
        Text(dc, deadlineX + Scale(4), plot.bottom + Scale(3), DimColor, L"%.1f ms (deadline)", s.frameMs);
        Text(dc, plot.right - Scale(120), plot.bottom + Scale(3), DimColor, L">= %.1f ms", 2 * s.frameMs);
    }

    void MainWindow::DrawLoadMetrics(HDC dc, const RECT& r) noexcept
    {
        Fill(dc, r, PanelColor);
        const LoadSnapshot& s = loadSnap_;
        SelectObject(dc, monoFont_);
        const std::int32_t line    = TextHeight(dc) + Scale(2);
        std::int32_t       x       = r.left + Scale(10);
        std::int32_t       y       = r.top + Scale(8);
        const double       missPct = s.frames > 0 ? 100.0 * static_cast<double>(s.misses) / static_cast<double>(s.frames) : 0;
        const double       due     = static_cast<double>(s.ticks) * s.streams;
        const double       cores   = s.frameMs > 0 ? s.streams * s.encodeUs / 1000.0 / s.frameMs : 0;
        Text(dc, x, y, TextColor, L"Running            %.1f s", s.seconds);
        y += line;
        Text(dc, x, y, TextColor, L"Frames due/encoded %.0f / %lld", due, s.frames);
        y += line;
        Text(dc, x, y, s.misses > 0 ? Red : Green, L"Deadline misses    %lld (%.3f %%)", s.misses, missPct);
        y += line;
        Text(dc, x, y, s.dropped > 0 ? Red : TextColor, L"Dropped frames     %lld (stream > %d frames behind)", s.dropped, 50);
        y += line;
        Text(dc, x, y, TextColor, L"Process CPU        %.1f %% of all logical processors (since start)", s.cpuPercent);
        y += line;

        x = (r.left + r.right) / 2;
        y = r.top + Scale(8);
        Text(dc, x, y, TextColor, L"Encode       avg %7.1f us per frame", s.encodeUs);
        y += line;
        Text(dc, x, y, TextColor, L"Encoder load %7.2f cores (streams x encode / frame)", cores);
        y += line;
        Text(dc, x, y, TextColor, L"Lateness ms  avg %.2f  p50 <%.2f  p99 <%.2f  max %.2f", s.latencyMs, s.p50Ms, s.p99Ms, s.maxMs);
        y += line;
        Text(dc, x, y, TextColor, L"Clock thread worst lateness %.2f ms (included above)", s.pacerLateMs);
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui
