// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

#include <cstdint>

namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus {

    /// <summary>
    /// Port of opusenc's main() (opus-tools 0.2 opusenc.c) for WAV input: same options, messages,
    /// exit codes and output. argvUtf8 holds the arguments as UTF-8 (WIN_UNICODE), argvWide the
    /// originals for file names. Not supported: --picture, --save-range and raw/AIFF/FLAC input.
    /// </summary>
    int RunFrontend(int argc, char** argvUtf8, wchar_t** argvWide);

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctOpus
