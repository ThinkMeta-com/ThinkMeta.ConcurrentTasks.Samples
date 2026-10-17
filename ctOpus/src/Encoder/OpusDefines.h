// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

// Generated from libopus 1.6.1 include/opus_defines.h and
// opus_multistream.h: the constants of the public Opus API that ctOpus uses, with their libopus names.

#include <cstdint>

inline constexpr std::int32_t OPUS_ALLOC_FAIL = -7;
inline constexpr std::int32_t OPUS_APPLICATION_AUDIO = 2049;
inline constexpr std::int32_t OPUS_APPLICATION_RESTRICTED_CELT = 2053;
inline constexpr std::int32_t OPUS_APPLICATION_RESTRICTED_LOWDELAY = 2051;
inline constexpr std::int32_t OPUS_APPLICATION_RESTRICTED_SILK = 2052;
inline constexpr std::int32_t OPUS_APPLICATION_VOIP = 2048;
inline constexpr std::int32_t OPUS_AUTO = -1000;
inline constexpr std::int32_t OPUS_BAD_ARG = -1;
inline constexpr std::int32_t OPUS_BANDWIDTH_FULLBAND = 1105;
inline constexpr std::int32_t OPUS_BANDWIDTH_MEDIUMBAND = 1102;
inline constexpr std::int32_t OPUS_BANDWIDTH_NARROWBAND = 1101;
inline constexpr std::int32_t OPUS_BANDWIDTH_SUPERWIDEBAND = 1104;
inline constexpr std::int32_t OPUS_BANDWIDTH_WIDEBAND = 1103;
inline constexpr std::int32_t OPUS_BITRATE_MAX = -1;
inline constexpr std::int32_t OPUS_BUFFER_TOO_SMALL = -2;
inline constexpr std::int32_t OPUS_FRAMESIZE_100_MS = 5008;
inline constexpr std::int32_t OPUS_FRAMESIZE_10_MS = 5003;
inline constexpr std::int32_t OPUS_FRAMESIZE_120_MS = 5009;
inline constexpr std::int32_t OPUS_FRAMESIZE_20_MS = 5004;
inline constexpr std::int32_t OPUS_FRAMESIZE_2_5_MS = 5001;
inline constexpr std::int32_t OPUS_FRAMESIZE_40_MS = 5005;
inline constexpr std::int32_t OPUS_FRAMESIZE_5_MS = 5002;
inline constexpr std::int32_t OPUS_FRAMESIZE_60_MS = 5006;
inline constexpr std::int32_t OPUS_FRAMESIZE_80_MS = 5007;
inline constexpr std::int32_t OPUS_FRAMESIZE_ARG = 5000;
inline constexpr std::int32_t OPUS_GET_APPLICATION_REQUEST = 4001;
inline constexpr std::int32_t OPUS_GET_BANDWIDTH_REQUEST = 4009;
inline constexpr std::int32_t OPUS_GET_BITRATE_REQUEST = 4003;
inline constexpr std::int32_t OPUS_GET_COMPLEXITY_REQUEST = 4011;
inline constexpr std::int32_t OPUS_GET_DTX_REQUEST = 4017;
inline constexpr std::int32_t OPUS_GET_EXPERT_FRAME_DURATION_REQUEST = 4041;
inline constexpr std::int32_t OPUS_GET_FINAL_RANGE_REQUEST = 4031;
inline constexpr std::int32_t OPUS_GET_FORCE_CHANNELS_REQUEST = 4023;
inline constexpr std::int32_t OPUS_GET_INBAND_FEC_REQUEST = 4013;
inline constexpr std::int32_t OPUS_GET_IN_DTX_REQUEST = 4049;
inline constexpr std::int32_t OPUS_GET_LOOKAHEAD_REQUEST = 4027;
inline constexpr std::int32_t OPUS_GET_LSB_DEPTH_REQUEST = 4037;
inline constexpr std::int32_t OPUS_GET_MAX_BANDWIDTH_REQUEST = 4005;
inline constexpr std::int32_t OPUS_GET_PACKET_LOSS_PERC_REQUEST = 4015;
inline constexpr std::int32_t OPUS_GET_PHASE_INVERSION_DISABLED_REQUEST = 4047;
inline constexpr std::int32_t OPUS_GET_PREDICTION_DISABLED_REQUEST = 4043;
inline constexpr std::int32_t OPUS_GET_QEXT_REQUEST = 4057;
inline constexpr std::int32_t OPUS_GET_SAMPLE_RATE_REQUEST = 4029;
inline constexpr std::int32_t OPUS_GET_SIGNAL_REQUEST = 4025;
inline constexpr std::int32_t OPUS_GET_VBR_CONSTRAINT_REQUEST = 4021;
inline constexpr std::int32_t OPUS_GET_VBR_REQUEST = 4007;
inline constexpr std::int32_t OPUS_INTERNAL_ERROR = -3;
inline constexpr std::int32_t OPUS_INVALID_PACKET = -4;
inline constexpr std::int32_t OPUS_OK = 0;
inline constexpr std::int32_t OPUS_RESET_STATE = 4028;
inline constexpr std::int32_t OPUS_SET_APPLICATION_REQUEST = 4000;
inline constexpr std::int32_t OPUS_SET_BANDWIDTH_REQUEST = 4008;
inline constexpr std::int32_t OPUS_SET_BITRATE_REQUEST = 4002;
inline constexpr std::int32_t OPUS_SET_COMPLEXITY_REQUEST = 4010;
inline constexpr std::int32_t OPUS_SET_DTX_REQUEST = 4016;
inline constexpr std::int32_t OPUS_SET_EXPERT_FRAME_DURATION_REQUEST = 4040;
inline constexpr std::int32_t OPUS_SET_FORCE_CHANNELS_REQUEST = 4022;
inline constexpr std::int32_t OPUS_SET_INBAND_FEC_REQUEST = 4012;
inline constexpr std::int32_t OPUS_SET_LSB_DEPTH_REQUEST = 4036;
inline constexpr std::int32_t OPUS_SET_MAX_BANDWIDTH_REQUEST = 4004;
inline constexpr std::int32_t OPUS_SET_PACKET_LOSS_PERC_REQUEST = 4014;
inline constexpr std::int32_t OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST = 4046;
inline constexpr std::int32_t OPUS_SET_PREDICTION_DISABLED_REQUEST = 4042;
inline constexpr std::int32_t OPUS_SET_QEXT_REQUEST = 4056;
inline constexpr std::int32_t OPUS_SET_SIGNAL_REQUEST = 4024;
inline constexpr std::int32_t OPUS_SET_VBR_CONSTRAINT_REQUEST = 4020;
inline constexpr std::int32_t OPUS_SET_VBR_REQUEST = 4006;
inline constexpr std::int32_t OPUS_SIGNAL_MUSIC = 3002;
inline constexpr std::int32_t OPUS_SIGNAL_VOICE = 3001;
inline constexpr std::int32_t OPUS_UNIMPLEMENTED = -5;
