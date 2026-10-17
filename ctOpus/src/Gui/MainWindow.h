// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

#include <Windows.h>

#include <cstdint>

#include "Audio.h"
#include "LiveSession.h"
#include "LoadTest.h"
#include "SignalSource.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui {

    /// <summary>Options for unattended runs (smoke tests): start right away, stop after a while, write a summary.</summary>
    struct CommandLine {
        bool         autostart        = false;
        bool         verify           = false; // check "bit-compare with libopus"
        std::int32_t engine           = 0;     // index in the encoder list
        bool         exclusive        = false; // check "WASAPI exclusive mode"
        bool         playback         = false; // play to the default output device
        bool         silent           = false; // ... but write silence to it
        std::int32_t loadStreams      = 0;     // > 0: start the multi-stream load with that many streams
        std::int32_t seconds          = 0;     // > 0: stop and close after that many seconds
        wchar_t      report[MAX_PATH] = {};
    };

    class MainWindow
    {
    public:
        MainWindow() noexcept;
        MainWindow(const MainWindow&)            = delete;
        MainWindow& operator=(const MainWindow&) = delete;
        ~MainWindow();

        bool         Create(HINSTANCE instance, std::int32_t show, const CommandLine& commandLine) noexcept;
        std::int32_t Run() noexcept;

    private:
        static constexpr std::int32_t MaxDevices  = 32;
        static constexpr std::int32_t LoadHistory = 400;

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
        LRESULT                 HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;

        void OnCreate() noexcept;
        void OnCommand(std::int32_t id, std::int32_t code) noexcept;
        void OnTimer() noexcept;
        void OnPaint() noexcept;
        void OnDpiChanged(std::int32_t dpi, const RECT* suggested) noexcept;

        HWND AddControl(const wchar_t* className, const wchar_t* text, unsigned long style, std::int32_t id) noexcept;
        void CreateFonts() noexcept;
        void Layout() noexcept;
        void FillModeDependentLists() noexcept;
        void RefreshDevices() noexcept;
        void UpdateControlState() noexcept;
        void UpdateComplexityLabel() noexcept;
        void SetStatus(const wchar_t* text) noexcept;
        void ShowDevicePeriods(const wchar_t* first) noexcept;
        void BrowseWav() noexcept;
        void StartStop() noexcept;
        bool StartLive() noexcept;
        bool StartLoad() noexcept;
        void StopAll() noexcept;
        void WriteReport() noexcept;

        bool         LoadMode() const noexcept;
        std::int32_t SelectedFrameSize() const noexcept;
        Application  SelectedApplication() const noexcept;
        std::int32_t SelectedBitrate() const noexcept;
        std::int32_t ComboIndex(HWND combo) const noexcept;
        std::int32_t EditNumber(HWND edit, std::int32_t fallback) const noexcept;
        std::int32_t Scale(std::int32_t value) const noexcept { return MulDiv(value, dpi_, 96); }

        // drawing
        void DrawLive(HDC dc, const RECT& area) noexcept;
        void DrawLoad(HDC dc, const RECT& area) noexcept;
        void DrawIdle(HDC dc, const RECT& area) noexcept;
        void DrawPanel(HDC dc, const RECT& r, const wchar_t* title) noexcept;
        void DrawWaveform(HDC dc, const RECT& r) noexcept;
        void DrawMeter(HDC dc, const RECT& r, const wchar_t* label, float db, float peak) noexcept;
        void DrawLatencyGraph(HDC dc, const RECT& r) noexcept;
        void DrawBudget(HDC dc, const RECT& r) noexcept;
        void DrawLiveMetrics(HDC dc, const RECT& r) noexcept;
        void DrawLoadGraph(HDC dc, const RECT& r) noexcept;
        void DrawHistogram(HDC dc, const RECT& r) noexcept;
        void DrawLoadMetrics(HDC dc, const RECT& r) noexcept;
        RECT VisualArea() const noexcept;

        HINSTANCE    instance_ = nullptr;
        HWND         hwnd_     = nullptr;
        std::int32_t dpi_      = 96;
        CommandLine  commandLine_;
        std::int64_t startedAt_ = 0;

        HFONT uiFont_    = nullptr;
        HFONT monoFont_  = nullptr;
        HFONT titleFont_ = nullptr;

        HWND modeLabel_        = nullptr;
        HWND mode_             = nullptr;
        HWND sourceLabel_      = nullptr;
        HWND source_           = nullptr;
        HWND browse_           = nullptr;
        HWND wavName_          = nullptr;
        HWND outputLabel_      = nullptr;
        HWND output_           = nullptr;
        HWND engineLabel_      = nullptr;
        HWND engine_           = nullptr;
        HWND frameLabel_       = nullptr;
        HWND frame_            = nullptr;
        HWND applicationLabel_ = nullptr;
        HWND application_      = nullptr;
        HWND bitrateLabel_     = nullptr;
        HWND bitrate_          = nullptr;
        HWND complexityLabel_  = nullptr;
        HWND complexity_       = nullptr;
        HWND streamsLabel_     = nullptr;
        HWND streams_          = nullptr;
        HWND threadsLabel_     = nullptr;
        HWND threads_          = nullptr;
        HWND lowLatency_       = nullptr;
        HWND exclusive_        = nullptr;
        HWND mmcss_            = nullptr;
        HWND verify_           = nullptr;
        HWND startStop_        = nullptr;
        HWND status_           = nullptr;

        AudioDevice  captureDevices_[MaxDevices] = {};
        std::int32_t captureCount_               = 0;
        AudioDevice  renderDevices_[MaxDevices]  = {};
        std::int32_t renderCount_                = 0;

        SignalBuffer testSignal_;
        SignalBuffer wav_;
        bool         wavLoaded_ = false;

        LiveSession  live_;
        LoadTest     load_;
        LiveSnapshot liveSnap_;
        LoadSnapshot loadSnap_;
        bool         running_                 = false;
        bool         runningLoad_             = false;
        float        inPeak_                  = -100;
        float        outPeak_                 = -100;
        float        loadMax_[LoadHistory]    = {};
        float        loadMisses_[LoadHistory] = {};
        std::int32_t loadHead_                = 0;
        std::int32_t loadCount_               = 0;
    };

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus::Gui
