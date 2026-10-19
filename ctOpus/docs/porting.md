# From libopus to ctOpus: where each function went

ctOpus is a port, rewritten in C++ and not linked: libopus 1.6.1, libopusenc 0.2.1 and the WAV reader
and frontend of opus-tools 0.2. This page maps the main functions of the originals to their ports, so
you can read both side by side. All ctOpus files are in `src\Encoder\` unless noted; the originals are
in `opus\`.

[← back to the README](../README.md)

## How to find the original of a function

Above every ported function a comment names its original and the file it comes from:

```cpp
// libopus: quant_all_bands() (celt/bands.c) (encoding)
void QuantAllBands(...)
```

The prefixes are `libopus:`, `libopusenc:` and `opus-tools:`. A ported function that does only part
of the original, or more, says so in the same comment (for example "the part of SignalStage" or
"with speex_resampler_skip_zeros()"). The SILK functions name their original (`silk_…`) in the
comment above each function.

## Naming

* **The ported state keeps the original names.** The members of the encoder states are libopus's
  fields in camelCase (`spread_decision` is `spreadDecision_`, `preemph_memE` is `preemphMemE_`); the
  SILK state keeps libopus's spelling exactly (`sLTP_shp_Q14`, `prev_gain_Q16`), Q-format suffix
  included.
* **Ported functions** get C++ names: `quant_all_bands()` is `QuantAllBands`, `ope_encoder_write_float()`
  is `OggOpusEncoder::WriteFloat`, `opus_custom_encoder_ctl()` is `CeltEncoder::Ctl`.
* **New structures get C++ names** of their own and no libopus name: the frames the analysis chain hands
  to the encoder (`CeltSignalFrame`, `CeltFrontFrame`, `StreamInputFrame`), the parallel resampler
  (`ParallelResampler`, `PlanarStore`), the speculation (`CeltSpeculation`, `QabInputs`, `QabJob`,
  `SeedPromise`, `SymbolLog`), the hook interfaces the task graph implements (`ToneAnalysisSource`,
  `ThetaTrialRunner`, `SpeculationRunner`) and everything in `src\Tasks\` and `src\App\`.
* **Integer helpers** of libopus (`IMIN`, `IMAX`) are `Imin` and `Imax` in `IntMath.h`, with the operand
  order of the macros kept.

## The map

`celt_encode_with_ec()` is split into stages, so that the parts that depend only on the signal can run
ahead on other tasks; the header of `CeltEncoder.h` lists which part of the original each stage holds,
and `CeltEncoder::Encode` marks the sections of the original with `// ---` comments.

| Original (file) | ctOpus (file) |
|---|---|
| `celt_encode_with_ec` (celt/celt_encoder.c) | `CeltEncoder::Encode` + `SignalStage` / `AnalyseSignal` + `Front` + `EncodeFrontSymbols` + `FinishEnergies` (CeltEncoder.cpp) |
| `celt_encoder_init`, `opus_custom_encoder_ctl` (celt/celt_encoder.c) | `CeltEncoder::Init`, `Ctl`, `Reset` (CeltEncoder.cpp) |
| `celt_preemphasis` (celt/celt_encoder.c) | `Preemphasis`, `CeltPreemphasis` (CeltEncoder.cpp) |
| `transient_analysis`, `patch_transient_decision` (celt/celt_encoder.c) | `TransientAnalysis`, `PatchTransientDecision` (CeltEncoder.cpp) |
| `tone_detect`, `tone_lpc` (celt/celt_encoder.c) | `ToneDetect`, `ToneLpc` (CeltEncoder.cpp) |
| `run_prefilter` (celt/celt_encoder.c) | `CeltEncoder::RunPrefilter` (CeltEncoder.cpp) |
| `compute_mdcts` (celt/celt_encoder.c) | `ComputeMdcts` (CeltEncoder.cpp) |
| `tf_analysis`, `tf_encode` (celt/celt_encoder.c) | `TfAnalysis`, `TfEncode` (CeltEncoder.cpp) |
| `dynalloc_analysis` (celt/celt_encoder.c) | `DynallocAnalysis` (CeltEncoder.cpp) |
| `alloc_trim_analysis`, `stereo_analysis` (celt/celt_encoder.c) | `AllocTrimAnalysis`, `StereoAnalysis` (CeltEncoder.cpp) |
| `compute_vbr` (celt/celt_encoder.c) | `ComputeVbr` (CeltEncoder.cpp) |
| `quant_all_bands` (celt/bands.c) | `QuantAllBands` (CeltBands.cpp) |
| `quant_band`, `quant_band_stereo`, `quant_partition`, `compute_theta` (celt/bands.c) | `QuantBand`, `QuantBandStereo`, `QuantPartition`, `ComputeTheta` (CeltBands.cpp) |
| `spreading_decision` (celt/bands.c) | `SpreadingDecision` (CeltBands.cpp) |
| `quant_coarse_energy`, `quant_fine_energy`, `quant_energy_finalise` (celt/quant_bands.c) | `QuantCoarseEnergy`, `QuantFineEnergy`, `QuantEnergyFinalise` (CeltEnergy.cpp) |
| `clt_compute_allocation` (celt/rate.c) | `ComputeAllocation` (CeltRate.cpp) |
| `alg_quant`, `op_pvq_search_sse2` (celt/vq.c, celt/x86/vq_sse2.c) | `AlgQuant`, `PvqSearch` (CeltVq.cpp) |
| `pitch_downsample`, `pitch_search`, `remove_doubling` (celt/pitch.c) | `PitchDownsample`, `PitchSearch`, `RemoveDoubling` (CeltPitch.cpp) |
| `comb_filter` (celt/celt.c) | `CombFilter` (CeltPitch.cpp) |
| `clt_mdct_forward_c` (celt/mdct.c), `opus_fft_impl` (celt/kiss_fft.c) | `MdctForward` (CeltMdct.cpp), `FftImpl` (KissFft.cpp) |
| `ec_enc_*` (celt/entenc.c) | `RangeEncoder` (RangeEncoder.cpp) |
| `tonality_analysis`, `run_analysis`, `tonality_get_info` (src/analysis.c) | `ToneAnalysis::AnalyzeFrame`, `Run` (+ `Advance`), `GetInfo` (+ `GetInfoFrom`) (ToneAnalysis.cpp) |
| `opus_encode_native`, `opus_encode_frame_native` (src/opus_encoder.c) | `OpusStreamEncoder::EncodeNative`, `EncodeFrameNative` (+ `CeltInputChain::Next`) (OpusStreamEncoder.cpp) |
| `opus_multistream_encode_native`, `surround_analysis` (src/opus_multistream_encoder.c) | `OpusMultistreamEncoder::EncodeFloat`, `SurroundAnalysis` (OpusMultistreamEncoder.cpp) |
| `ope_encoder_write_float`, `encode_buffer`, `ope_encoder_drain` (libopusenc src/opusenc.c) | `OggOpusEncoder::WriteFloat`, `EncodeBuffer`, `Drain` (OggOpusEncoder.cpp) |
| `oggp_*` (libopusenc src/ogg_packer.c) | `OggPacker` (OggPacker.cpp) |
| `speex_resampler_*`, `resampler_basic_direct_single` (libopusenc src/resample.c) | `Resampler::*`, `Resampler::Kernel` (Resampler.cpp); the parallel form, ctOpus's own: `ParallelResampler` (ParallelResampler.cpp) |
| `wav_open`, `wav_read` (opus-tools src/audio-in.c) | `WavInput::Open`, `WavInput::ReadPcm` (WavInput.cpp) |
| the opusenc program (opus-tools src/opusenc.c) | the command line in `CommandLine::Parse` (`src\App\CommandLine.cpp`), the rest of `main()` in `Application::Execute` (`src\App\Application.cpp`) |
| (ctOpus's own) the speculation | `CeltEncoder::BuildSpeculation`, `RunSpeculation`, `CeltEncoder::TakeSpeculation`, `CeltEncoder::SpeculateNext`, `RunQabJob` (CeltSpeculation.cpp) |
| SILK (silk/, silk/float/) | the `Silk…` files; each function names its `silk_…` original in the comment above it |

The task graph (`src\Tasks\`) has no original: opusenc runs everything on one thread. How it grew
is told in [task-graph.md](task-graph.md).

[← back to the README](../README.md)
