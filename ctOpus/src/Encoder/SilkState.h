// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)
// Part of ctOpus, a hand-written C++ port of libopus 1.6.1, libopusenc 0.2.1 and opus-tools 0.2;
// their BSD license notices are in COPYING.

#pragma once

/* The state of the SILK encoder of libopus 1.6.1 (silk/structs.h, silk/float/structs_FLP.h,
   silk/resampler_structs.h, silk/control.h), float build, with the
 * layout and the field names
   of libopus (during the port, LibopusBridge.c checked every field offset against libopus while
   ported and libopus functions
 * shared the state). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SilkEncoderNumChannels   = 2,
    SilkMaxFramesPerPacket   = 3,
    SilkMaxNbSubfr           = 4,
    SilkMaxFsKhz             = 16,
    SilkMaxSubFrameLength    = 5 * SilkMaxFsKhz,
    SilkMaxFrameLength       = 20 * SilkMaxFsKhz,
    SilkLaShapeMax           = 5 * SilkMaxFsKhz,
    SilkMaxLpcOrder          = 16,
    SilkNsqLpcBufLength      = SilkMaxLpcOrder,
    SilkMaxShapeLpcOrder     = 24,
    SilkLtpOrder             = 5,
    SilkVadNBands            = 4,
    SilkResamplerMaxIirOrder = 6,
    SilkResamplerMaxFirOrder = 36
};

typedef struct SilkNsqState {
    int16_t xq[2 * SilkMaxFrameLength]; /* the quantized output signal */
    int32_t sLTP_shp_Q14[2 * SilkMaxFrameLength];
    int32_t sLPC_Q14[SilkMaxSubFrameLength + SilkNsqLpcBufLength];
    int32_t sAR2_Q14[SilkMaxShapeLpcOrder];
    int32_t sLF_AR_shp_Q14;
    int32_t sDiff_shp_Q14;
    int     lagPrev;
    int     sLTP_buf_idx;
    int     sLTP_shp_buf_idx;
    int32_t rand_seed;
    int32_t prev_gain_Q16;
    int     rewhite_flag;
} SilkNsqState;

typedef struct SilkVadState {
    int32_t AnaState[2];  /* analysis filterbank state: 0-8 kHz */
    int32_t AnaState1[2]; /* 0-4 kHz */
    int32_t AnaState2[2]; /* 0-2 kHz */
    int32_t XnrgSubfr[SilkVadNBands];
    int32_t NrgRatioSmth_Q8[SilkVadNBands];
    int16_t HPstate; /* differentiator in the lowest band */
    int32_t NL[SilkVadNBands];
    int32_t inv_NL[SilkVadNBands];
    int32_t NoiseLevelBias[SilkVadNBands];
    int32_t counter; /* frame counter of the initial phase */
} SilkVadState;

typedef struct SilkLpState {
    int32_t In_LP_State[2];
    int32_t transition_frame_no; /* mapped to a cut-off frequency */
    int     mode;                /* <0: switch down, >0: switch up, 0: nothing */
    int32_t saved_fs_kHz;        /* the last rate before a bandwidth switching reset, if non-zero */
} SilkLpState;

typedef struct SilkNlsfCb {
    int16_t        nVectors;
    int16_t        order;
    int16_t        quantStepSize_Q16;
    int16_t        invQuantStepSize_Q6;
    const uint8_t* CB1_NLSF_Q8;
    const int16_t* CB1_Wght_Q9;
    const uint8_t* CB1_iCDF;
    const uint8_t* pred_Q8;
    const uint8_t* ec_sel;
    const uint8_t* ec_iCDF;
    const uint8_t* ec_Rates_Q5;
    const int16_t* deltaMin_Q15;
} SilkNlsfCb;

typedef struct SilkStereoEncState {
    int16_t pred_prev_Q13[2];
    int16_t sMid[2];
    int16_t sSide[2];
    int32_t mid_side_amp_Q0[4];
    int16_t smth_width_Q14;
    int16_t width_prev_Q14;
    int16_t silent_side_len;
    int8_t  predIx[SilkMaxFramesPerPacket][2][3];
    int8_t  mid_only_flags[SilkMaxFramesPerPacket];
} SilkStereoEncState;

typedef struct SilkSideInfoIndices {
    int8_t  GainsIndices[SilkMaxNbSubfr];
    int8_t  LTPIndex[SilkMaxNbSubfr];
    int8_t  NLSFIndices[SilkMaxLpcOrder + 1];
    int16_t lagIndex;
    int8_t  contourIndex;
    int8_t  signalType;
    int8_t  quantOffsetType;
    int8_t  NLSFInterpCoef_Q2;
    int8_t  PERIndex;
    int8_t  LTP_scaleIndex;
    int8_t  Seed;
} SilkSideInfoIndices;

typedef struct SilkResamplerState {
    int32_t sIIR[SilkResamplerMaxIirOrder]; /* must be the first element */
    union
    {
        int32_t i32[SilkResamplerMaxFirOrder];
        int16_t i16[SilkResamplerMaxFirOrder];
    } sFIR;
    int16_t        delayBuf[96];
    int            resampler_function;
    int            batchSize;
    int32_t        invRatio_Q16;
    int            FIR_Order;
    int            FIR_Fracs;
    int            Fs_in_kHz;
    int            Fs_out_kHz;
    int            inputDelay;
    const int16_t* Coefs;
} SilkResamplerState;

typedef struct SilkEncoderState {
    int32_t             In_HP_State[2];
    int32_t             variable_HP_smth1_Q15;
    int32_t             variable_HP_smth2_Q15;
    SilkLpState         sLP;
    SilkVadState        sVAD;
    SilkNsqState        sNSQ;
    int16_t             prev_NLSFq_Q15[SilkMaxLpcOrder];
    int                 speech_activity_Q8;
    int                 allow_bandwidth_switch;
    int8_t              LBRRprevLastGainIndex;
    int8_t              prevSignalType;
    int                 prevLag;
    int                 pitch_LPC_win_length;
    int                 max_pitch_lag;
    int32_t             API_fs_Hz;
    int32_t             prev_API_fs_Hz;
    int                 maxInternal_fs_Hz;
    int                 minInternal_fs_Hz;
    int                 desiredInternal_fs_Hz;
    int                 fs_kHz;
    int                 nb_subfr;
    int                 frame_length;
    int                 subfr_length;
    int                 ltp_mem_length;
    int                 la_pitch;
    int                 la_shape;
    int                 shapeWinLength;
    int32_t             TargetRate_bps;
    int                 PacketSize_ms;
    int                 PacketLoss_perc;
    int32_t             frameCounter;
    int                 Complexity;
    int                 nStatesDelayedDecision;
    int                 useInterpolatedNLSFs;
    int                 shapingLPCOrder;
    int                 predictLPCOrder;
    int                 pitchEstimationComplexity;
    int                 pitchEstimationLPCOrder;
    int32_t             pitchEstimationThreshold_Q16;
    int32_t             sum_log_gain_Q7;
    int                 NLSF_MSVQ_Survivors;
    int                 first_frame_after_reset;
    int                 controlled_since_last_payload;
    int                 warping_Q16;
    int                 useCBR;
    int                 prefillFlag;
    const uint8_t*      pitch_lag_low_bits_iCDF;
    const uint8_t*      pitch_contour_iCDF;
    const SilkNlsfCb*   psNLSF_CB;
    int                 input_quality_bands_Q15[SilkVadNBands];
    int                 input_tilt_Q15;
    int                 SNR_dB_Q7;
    int8_t              VAD_flags[SilkMaxFramesPerPacket];
    int8_t              LBRR_flag;
    int                 LBRR_flags[SilkMaxFramesPerPacket];
    SilkSideInfoIndices indices;
    int8_t              pulses[SilkMaxFrameLength];
    int                 arch;
    int16_t             inputBuf[SilkMaxFrameLength + 2];
    int                 inputBufIx;
    int                 nFramesPerPacket;
    int                 nFramesEncoded;
    int                 nChannelsAPI;
    int                 nChannelsInternal;
    int                 channelNb;
    int                 frames_since_onset;
    int                 ec_prevSignalType;
    int16_t             ec_prevLagIndex;
    SilkResamplerState  resampler_state;
    int                 useDTX;
    int                 inDTX;
    int                 noSpeechCounter;
    int                 useInBandFEC;
    int                 LBRR_enabled;
    int                 LBRR_GainIncreases;
    SilkSideInfoIndices indices_LBRR[SilkMaxFramesPerPacket];
    int8_t              pulses_LBRR[SilkMaxFramesPerPacket][SilkMaxFrameLength];
} SilkEncoderState;

typedef struct SilkShapeStateFlp {
    int8_t LastGainIndex;
    float  HarmShapeGain_smth;
    float  Tilt_smth;
} SilkShapeStateFlp;

typedef struct SilkEncoderStateFlp {
    SilkEncoderState  sCmn;
    SilkShapeStateFlp sShape;
    float             x_buf[2 * SilkMaxFrameLength + SilkLaShapeMax]; /* for the pitch and noise shape analysis */
    float             LTPCorr;                                        /* normalized correlation of the pitch lag estimator */
} SilkEncoderStateFlp;

typedef struct SilkEncoderControlFlp {
    float   Gains[SilkMaxNbSubfr];
    float   PredCoef[2][SilkMaxLpcOrder]; /* interpolated and final coefficients */
    float   LTPCoef[SilkLtpOrder * SilkMaxNbSubfr];
    float   LTP_scale;
    int     pitchL[SilkMaxNbSubfr];
    float   AR[SilkMaxNbSubfr * SilkMaxShapeLpcOrder];
    float   LF_MA_shp[SilkMaxNbSubfr];
    float   LF_AR_shp[SilkMaxNbSubfr];
    float   Tilt[SilkMaxNbSubfr];
    float   HarmShapeGain[SilkMaxNbSubfr];
    float   Lambda;
    float   input_quality;
    float   coding_quality;
    float   predGain;
    float   LTPredCodGain;
    float   ResNrg[SilkMaxNbSubfr];
    int32_t GainsUnq_Q16[SilkMaxNbSubfr];
    int8_t  lastGainIndexPrev;
} SilkEncoderControlFlp;

typedef struct SilkEncoder {
    SilkStereoEncState  sStereo;
    int32_t             nBitsUsedLBRR;
    int32_t             nBitsExceeded;
    int                 nChannelsAPI;
    int                 nChannelsInternal;
    int                 nPrevChannelsInternal;
    int                 timeSinceSwitchAllowed_ms;
    int                 allowBandwidthSwitch;
    int                 prev_decode_only_middle;
    SilkEncoderStateFlp state_Fxx[SilkEncoderNumChannels]; /* last: mono skips the second state */
} SilkEncoder;

/* silk_EncControlStruct (control.h) */
typedef struct SilkEncControl {
    int32_t nChannelsAPI;
    int32_t nChannelsInternal;
    int32_t API_sampleRate;
    int32_t maxInternalSampleRate;
    int32_t minInternalSampleRate;
    int32_t desiredInternalSampleRate;
    int     payloadSize_ms;
    int32_t bitRate;
    int     packetLossPercentage;
    int     complexity;
    int     useInBandFEC;
    int     useDRED;
    int     LBRR_coded;
    int     useDTX;
    int     useCBR;
    int     maxBits;
    int     toMono;
    int     opusCanSwitch;
    int     reducedDependency;
    int32_t internalSampleRate;
    int     allowBandwidthSwitch;
    int     inWBmodeWithoutVariableLP;
    int     stereoWidth_Q14;
    int     switchReady;
    int     signalType;
    int     offset;
} SilkEncControl;

#ifdef __cplusplus
}
#endif
