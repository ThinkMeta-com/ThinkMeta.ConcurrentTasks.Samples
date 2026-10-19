# From LAME to ctLame: where each function went

ctLame's encoder (`src\Encoder\`) is a hand-written C++ port of the reduced LAME 4.0 in `lame\`. The
functions are rewritten, not copied, and their output is checked byte for byte against LAME; only
constant tables are taken over as data, marked as such in the source. This page maps LAME's
functions to the files and functions of the port, so you can read both side by side.

[← back to the README](../README.md)

## Names

The naming convention of the encoder (also at the top of `src\Encoder\SessionConfig.h`):

- **State and configuration ported from LAME keep LAME's names**, snake_case included
  (`SessionConfig::samplerate_out`, `GrInfo::part2_3_length`, `PsyState::nb_l1`, `ATH_offset_db`),
  so a field can be looked up in LAME's sources as it is.
- **What ctLame adds has C++ names:** functions, classes and structures (`FrameData`, `PsyPrep`,
  `PsyGranuleOutput`, `ReservoirState`), their members (camelCase) and local variables.
- **A ported function has a C++ name and names its LAME original** in its comment or summary
  ("LAME: pecalc_s() (psymodel.c)" or "pecalc_s: ..."); constants of LAME keep their names
  (`SBMAX_l`, `FFTOFFSET`, `NS_PREECHO_ATT0`).

## Where the port differs in shape

Where LAME has one function for work with and without state, ctLame splits it along that line, so
the part without state can run for many frames at once (see the [task graph](task-graph.md)):

- `lame_encode_mp3_frame` became the four calls of `Encoder` for the stages 2 to 5 (Prepare,
  Resolve, Search, Encode); `Encoder::RunAllStages` runs them on one frame after the other.
- `L3psycho_anal_vbr` became `PsyPrepare` (without state) and `PsyResolve` (with state);
  `PsyAnalyze` runs both for one granule.
- `mdct_sub48` became the polyphase filterbank (`MdctPolyphaseFrame`), the carry from frame to
  frame (`MdctCarry`) and the MDCT (`MdctFromPolyphase`).
- `VBR_new_iteration_loop` became `QuantSearch` (without the reservoir) and `QuantCommit` (with
  it); `VBR_encode_frame` became `VbrSearchFrame` and `VbrFitFrame` in the same way.

Two places stay close to LAME's code on purpose, and their comments say why: the band loop of
`mdct_sub48` (`MdctChannelFrom` in `Mdct.cpp`), whose index arithmetic is the definition of the
folding, and the short block pre-echo control in `PsyResolve.cpp`, whose interplay of thresholds is
LAME's tuning. The SIMD kernels (`…Simd.inc`) compute the same operations in the same order as the
scalar code they sit next to.

## Encoder

| LAME (`lame\libmp3lame\`) | ctLame (`src\Encoder\`) |
|---|---|
| `lame_encode_mp3_frame` (encoder.c) | `Encoder.cpp`: `Encoder::Prepare`, `Resolve`, `Search`, `Encode`, `RunAllStages`; the stages in `Stages.cpp` |
| `lame_encode_frame_init` (encoder.c) | `Mdct.cpp`: `MdctPrime`, `MdctPrimeCarry` |
| `adjust_ATH` (encoder.c) | `PsyResolve.cpp`: `AdjustAth` |
| `mdct_sub48` (newmdct.c) | `Mdct.cpp`: `MdctChannelFrom`, `MdctPolyphaseFrame`, `MdctCarry`, `MdctFromPolyphase` |
| `window_subband` (newmdct.c) | `MdctKernels.cpp`: `WindowSubband`, `WindowSubbandTail` (SIMD in `MdctSimd.inc`) |
| `mdct_short`, `mdct_long` (newmdct.c) | `MdctKernels.cpp`: `MdctShort`, `MdctLong` |
| `fht`, `fft_long`, `fft_short`, `init_fft` (fft.c) | `Fft.cpp`: `FhtScalar`, `FftLong`, `FftShort`, `FftWindows::Initialize` |
| `psymodel_init` (psymodel.c) | `PsyInit.cpp`: `PsyInit` |
| `init_numline`, `compute_bark_values`, `init_s3_values`, `s3_func` (psymodel.c) | `PsyInit.cpp`: `InitNumline`, `ComputeBarkValues`, `InitS3Values`, `SpreadingFunction` |
| `L3psycho_anal_vbr` (psymodel.c) | `PsyPrepare.cpp`: `PsyPrepare` and `PsyResolve.cpp`: `PsyResolve` (together `PsyAnalyze`) |
| `vbrpsy_attack_detection` (psymodel.c) | `PsyPrepare.cpp`: `AttackPrepScalar` and `PsyResolve.cpp`: `AttackResolve` |
| `vbrpsy_compute_fft_l`, `vbrpsy_compute_fft_s` (psymodel.c) | `PsyPrepare.cpp`: `ComputeFftLong`, `PsyDetail::ComputeFftShort` |
| `vbrpsy_compute_masking_l` (psymodel.c) | `PsyPrepare.cpp`: `SpreadLong` and `PsyResolve.cpp`: `MaskingLongResolve`, `MaskingLongScalar` |
| `vbrpsy_compute_masking_s` (psymodel.c) | `PsyPrepare.cpp`: `PsyDetail::SpreadShort` and `PsyResolve.cpp`: `MaskingShortResolve` |
| `vbrpsy_mask_add` (psymodel.c) | `PsyPrepare.cpp`: `MaskAdd`, `MaskChainsScalar` |
| `vbrpsy_compute_MS_thresholds` (psymodel.c) | `PsyResolve.cpp`: `ComputeMsThresholds` |
| `convert_partition2scalefac`, `_s`, `_l`, `_l_to_s` (psymodel.c) | `PsyResolve.cpp`: `PartitionsToScalefactorBands`, `PartitionsToShortBands`, `PartitionsToLongBands`, `LongPartitionsToShortBands` |
| `pecalc_s`, `pecalc_l` (psymodel.c) | `PsyResolve.cpp`: `PerceptualEntropyShort`, `PerceptualEntropyLong` |
| `iteration_init`, `compute_ath` (quantize_pvt.c) | `Quantize.cpp`: `QuantizeInit`, `ComputeAth` |
| `calc_xmin` (quantize_pvt.c) | `Quantize.cpp`: `CalcXmin` (with `BandXmin`) |
| `VBR_new_iteration_loop` (quantize.c) | `Quantize.cpp`: `QuantSearch` and `QuantCommit` (with `VbrNewPrepare`, `OnPe`) |
| `VBR_encode_frame` (vbrquantize.c) | `VbrQuantize.cpp`: `VbrSearchFrame` and `VbrFitFrame` (with `DistributeBudget`, `FitGranules`) |
| `block_sf`, `find_scalefac_x34`, `outOfBitsStrategy` (vbrquantize.c) | `VbrQuantize.cpp`: `BlockSf`, `FindScalefac`, `OutOfBitsStrategy` |
| `noquant_count_bits`, `choose_table` (takehiro.c) | `BitCount.cpp`: `CountBits`, `ChooseTable` |
| `count_bit_noESC`, `count_bit_noESC_from2`, `count_bit_noESC_from3`, `count_bit_ESC` (takehiro.c) | `BitCount.cpp`: `CountTable1`, `CountTablePair`, `CountTableTriple`, `CountTableEscape` |
| `best_huffman_divide`, `best_scalefac_store`, `mpeg1_scale_bitcount` (takehiro.c) | `BitCount.cpp`: `BestHuffmanDivide`, `BestScalefacStore`, `ScaleBitcount` |
| `ResvFrameBegin`, `ResvMaxBits`, `ResvFrameEnd` (reservoir.c) | `Reservoir.cpp`: `ReservoirFrameBegin`, `ReservoirMaxBits`, `ReservoirFrameEnd` |
| `format_bitstream`, `writeMainData`, `flush_bitstream` (bitstream.c) | `Bitstream.cpp`: `FormatBitstream`, `PrepareMainData` and `WriteMainData`, `FlushBitstream` |
| `InitVbrTag`, `lame_get_lametag_frame` (VbrTag.c) | `VbrTag.cpp`: `InitVbrTag`, `GetLameTagFrame` |
| `lame_init_params` (lame.c) | `SessionConfig.cpp`: `InitParams` |

## Frontend and input

| LAME (`lame\frontend\`, `lame\libmp3lame\`) | ctLame |
|---|---|
| `main.c`, `lame_main.c` | `src\App\main.cpp`; `lame_encoder_loop` is `RunEncodeGraph`, which runs the [task graph](task-graph.md) |
| `parse.c`: `parse_args`, `usage`, `display_bitrates` | `src\App\CommandLine.cpp`: `ParseArgs`, `Usage`, `DisplayBitrates` |
| `get_audio.c`: opening the WAV file and parsing its header | `src\App\WavFile.cpp`: `WavFile` |
| `get_audio.c`: reading the samples | `src\Encoder\Input.cpp`: `PcmReader` |
| the input buffering: `lame_copy_inbuffer`, `lame_encode_buffer_sample_t`, `fill_buffer`, `lame_encode_flush` | `src\Encoder\SampleStream.cpp`: `SampleStream` |
| `lame_global_flags` with the defaults of `lame_init` | `src\Encoder\EncoderOptions.h`: `EncoderOptions` |

[← back to the README](../README.md)
