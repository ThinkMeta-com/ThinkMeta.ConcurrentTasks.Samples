# From jpegli to ctJpeg: where each function went

ctJpeg is a new C++ port of jpegli's encoder (google/jpegli @ 031a007, `jpeg\jpegli\lib\jpegli\`),
not a copy. This page maps jpegli's functions to the places in ctJpeg that port them, so a reader
can put both side by side. The ported code is in `src\Encoder\`; it knows nothing of tasks. The
task graph (`src\Tasks\`) and the command line (`src\App\`) are new.

[← back to the README](../README.md)

## Naming

* **Ported functions keep jpegli's names** where they can: jpegli is C++ itself, so
  `SetQuantMatrices`, `InitQuantizer`, `CreateHuffmanTree`, `ClusterJpegHistograms`,
  `OptimizeHuffmanCodes`, `TokenizeScan`, `WriteScanData`, `FastLog2f` and many more have the same
  name in both. Every ported function has a comment above it (or in its `<summary>`) that names
  the original: `// jpegli: Name() (file.cc)`. Where the name differs, that comment is the link
  (`IsEmpty` is `IsEmptyHistogram()`, `TreeLess` is `Compare()`, the vector kernels end in `V`).
* **Ported state keeps jpegli's meaning**, with ctJpeg's spelling of names: the fields of
  `EncoderSetup` (`EncoderSetup.h`) carry the name of the jpegli field they hold in a comment, for
  example `acContextOffset // m->ac_ctx_offset` or `iMcuRows // total_iMCU_rows`; `Token`,
  `RefToken` and `Histograms` (`Entropy.h`) are jpegli's `Token`, `RefToken` and `Histogram`.
* **New structures get names of their own** and a comment `New in ctJpeg, not in jpegli`: the
  tiles of the pixel phase, the band split of the tokenization (`BandStitch.cpp`), the chunked
  writing (`ChunkSplice.cpp`), the bit masks of the tokenizer, the SIMD self-test, and the whole
  task layer.

All file names below are in `src\Encoder\` unless a folder is given; jpegli's files are in
`jpeg\jpegli\lib\jpegli\`.

## Setup: parameters, quantization, scan script

| jpegli | ctJpeg |
|---|---|
| `encode.cc`: `jpegli_set_defaults()`, `jpegli_set_colorspace()`, `jpegli_set_distance()`, `ProcessCompressionParams()`, and `quant.cc`: `InitQuantizer()`, as cjpegli calls them | `EncoderSetup.cpp`: `SetupEncoder` (fills `EncoderSetup`) |
| `encode.cc`: `jpegli_quality_to_distance()` | `EncoderSetup.cpp`: `QualityToDistance` |
| `encode.cc`: `SetDefaultScanScript()` (level 2) and the AC context offsets of `ProcessCompressionParams()` | `EncoderSetup.cpp`: `SetScanScript` |
| `quant.cc`: `SetQuantMatrices()` | `EncoderSetup.cpp`: `SetQuantMatrices` |
| `quant.cc`: `InitQuantizer()` (multipliers and zero bias) | `EncoderSetup.cpp`: `InitQuantizer` |
| `quant.cc`: `DistanceToScale()`, `ScaleToDistance()`, `QuantValsToDistance()` | `EncoderSetup.cpp`: `DistanceToScale`, `ScaleToDistance`, `QuantValsToDistance` |
| `quant.cc`: the data tables of the quantizer | `JpegliTables.cpp` (generated from the source text) |

## Pixel phase: color, adaptive quantization, DCT, quantization

The pixel phase is one source, `PixelPhaseImpl.inc`, compiled once per feature set
(`PixelPhaseSse2.cpp`, `PixelPhaseAvx2.cpp`); `PixelPhase.cpp` chooses one. The scalar references
of the SIMD kernels are in `PixelPhaseReference.inc`.

| jpegli | ctJpeg |
|---|---|
| `encode.cc`: `ProcessiMCURows()` and the functions it calls | `PixelPhase.cpp`: `ComputeCoefficients` |
| `encode.cc`: `ProcessiMCURows()` and `ProcessiMCURow()`, for one tile | `PixelPhaseImpl.inc`: `ComputeBand` |
| `encode.cc`: `ReadInputRow()`, `PadInputBuffer()`; `color_transform.cc`: `ExtRGBToYCbCr()` | `PixelPhaseImpl.inc`: `ReadInputRows` |
| `input.cc`: `ReadUint8RowInterleaved3()`; `color_transform.cc`: `ExtRGBToYCbCr()`, for one row | `PixelPhaseImpl.inc`: `ColorRowV` |
| `downsample.cc`: `Downsample2x2()` (4:2:0) | `PixelPhaseImpl.inc`: `Downsample2x2Rows` |
| `adaptive_quantization.cc`: `ComputeAdaptiveQuantField()` | `PixelPhaseImpl.inc`: `ComputeAdaptiveQuantField` |
| `adaptive_quantization.cc`: `ComputePreErosion()`; `common_internal.h`: `RowBuffer::PadRow()` | `PixelPhaseImpl.inc`: `ComputePreErosionRows`, `PreErosionRowV`; reference `PreErosionRow` |
| `adaptive_quantization.cc`: `Sort4()`, `UpdateMin4()`, `FuzzyErosion()` | `PixelPhaseImpl.inc`: `FuzzyErosionBlockRow`, `FuzzyErosionRowV`; reference `FuzzyErosionRow` |
| `adaptive_quantization.cc`: `PerBlockModulations()` and the final mapping of `ComputeAdaptiveQuantField()` | `PixelPhaseImpl.inc`: `PerBlockModulationsRow`; the dampening at its start: `ComputeQuantFieldScaling` |
| `adaptive_quantization.cc`: `HfModulation()`, `GammaModulation()` | `PixelPhaseImpl.inc`: `BlockModulationsV`; reference `BlockModulations` |
| `adaptive_quantization.cc`: `ComputeMask()`, `FastLog2f()`, `FastPow2f()` | `PixelPhaseImpl.inc`: `ComputeMask`, `FastLog2f`, `FastPow2f` |
| `adaptive_quantization.cc`: `RatioOfDerivativesOfCubicRootToSimpleGamma()` | `PixelPhaseImpl.inc`: `RatioV`; reference `RatioOfDerivatives` |
| `adaptive_quantization.cc`: `MaskingSqrt()` | `PixelPhaseReference.inc`: `MaskingSqrt` |
| `encode_streaming.cc`: `ProcessiMCURow<kStreamingModeCoefficients>()` (DCT and quantization of an iMCU row) | `PixelPhaseImpl.inc`: `ComputeCoefficientsForiMcuRow` |
| `dct-inl.h`: `ComputeCoefficientBlock()`, `TransformFromPixels()`, `QuantizeBlock()` | `PixelPhaseImpl.inc`: `QuantizeBlockV`; references `TransformFromPixels`, `QuantizeBlockTo` |
| `dct-inl.h`: `DCT1DImpl<N>`, `DCT1D()` | `PixelPhaseImpl.inc`: `DctV<L, N>`, `Dct1DColumnsV`; reference `Dct1DImpl<L, N>` |
| `transpose-inl.h`: `Transpose8x8Block()` | `PixelPhaseImpl.inc`: `Transpose8x8` |
| Highway's arithmetic per target (fused or separate multiply-add, lane order of sums) | `VecMath.h` (`Vec<L>`), `LaneMath.h` (`Lane<L>`) |
| jpegli's `coeff_buffers` | `Coefficients.h`: `Coefficients` (plus a dense DC plane, new in ctJpeg) |

New in ctJpeg: the tiles (`ComputeBand` reads the context rows and columns of its tile itself, so
tiles run in parallel and give jpegli's values) and the kernel self-test
(`PixelPhaseReference.inc`, run by `src\Tests\KernelTests.cpp`).

## Entropy coding: tokens, Huffman codes, bit stream

| jpegli | ctJpeg |
|---|---|
| `entropy_coding.cc`: `TokenizeScan()` | `Tokenize.cpp`: `TokenizeScan` |
| `entropy_coding.cc`: `TokenizeProgressiveDC()` and the DC loop of `TokenizeScan()` | `Tokenize.cpp`: `TokenizeDcRows` |
| `entropy_coding.cc`: `TokenizeACProgressiveScan()` | `Tokenize.cpp`: `TokenizeAcFirstRows`; its `emit_eob_run`: `Tokenize.h`: `EobRunToken` |
| `entropy_coding.cc`: `TokenizeACRefinementScan()` | `Tokenize.cpp`: `TokenizeAcRefinementRows` |
| `entropy_coding.cc`: `BuildHistograms()` | `Tokenize.cpp`: `AddScanHistograms` |
| `entropy_coding.cc`: `OptimizeHuffmanCodes()` | `HuffmanCodes.cpp`: `OptimizeHuffmanCodes` |
| `entropy_coding.cc`: `ClusterJpegHistograms()` (result `JpegClusteredHistograms`) | `HuffmanCodes.cpp`: `ClusterJpegHistograms` (result `Clusters`) |
| `entropy_coding.cc`: `HistogramCost()`, `IsEmptyHistogram()`, `kNumExtraBits` | `HuffmanCodes.cpp`: `HistogramCost`, `IsEmpty`, `NumExtraBits` |
| `entropy_coding.cc`: `BuildJpegHuffmanTable()`, `BuildHuffmanCodeTable()` | `HuffmanCodes.cpp`: `BuildJpegHuffmanTable`, `BuildHuffmanCodeTable` |
| `huffman.cc`: `CreateHuffmanTree()`, `SetDepth()`, `Compare()`, `HuffmanTree` | `HuffmanCodes.cpp`: `CreateHuffmanTree`, `SetDepth`, `TreeLess`, `TreeNode` |
| `bitstream.cc`: `WriteScanData()` | `ScanWriter.cpp`: `WriteScanData` |
| `bitstream.cc`: `WriteScanHeader()` | `ScanWriter.cpp`: `WriteScanHeader` |
| `bitstream.cc`: `WriteTokens()`, `WriteACRefinementTokens()` | `ScanWriter.h`: `WriteTokenParts` |
| `bit_writer.h`: `JpegBitWriter`, `DischargeBitBuffer()`, `EmitByte()`; `bit_writer.cc`: `JumpToByteBoundary()` | `BitWriter.h`/`.cpp`: `BitWriter`, `DischargeBitBuffer`, `EmitByte`, `JumpToByteBoundary` |
| `bit_writer.h`: `WriteBits()`, `HasZeroByte()` | `BitPacking.h`: `PackBits`, `HasZeroByte` |

New in ctJpeg (no jpegli counterpart; see [How the graph grew](task-graph.md), steps 4 and 6):

| ctJpeg | What it does |
|---|---|
| `BandStitch.cpp`: `TokenizeScanBand`, `TokenizeDcBand`, `StitchScanBands` | a scan tokenized in bands of block rows; the stitch replays the end-of-band state machine of `TokenizeACRefinementScan()` across the band borders and gives exactly jpegli's tokens |
| `ChunkSplice.cpp`: `CountRefinementStreams`, `LocateRefinementStreams`, `WriteScanChunk`, `SpliceScanChunk` | a scan written in chunks of plain bits; the splice joins them to the stuffed bytes `WriteScanData` writes for the whole scan |
| `Tokenize.cpp`: `ComputeMasks` | the non-zero coefficients of a block as bit masks, so the tokenizers jump from one to the next |

## Markers and the file

| jpegli | ctJpeg |
|---|---|
| `encode.cc`: `jpegli_finish_compress()`: the file layout SOI, DQT, SOF, per scan DHT/SOS and data, EOI | `JpegFile.h`: `AssembleJpeg` (used by the sequential encoder and by the node Assemble of the graph) |
| `encode.cc`: the SOI of `InitCompress()`, the EOI of `jpegli_finish_compress()` | `Markers.cpp`: `WriteSoi`, `WriteEoi` |
| `bitstream.cc`: `EncodeDQT()`, `EncodeSOF()`, `EncodeDHT()`, `EncodeSOS()` | `Markers.cpp`: `WriteDqt`, `WriteSof`, `WriteDht`, `WriteSos` |

The sequence of the whole encoder, as jpegli runs it on one thread, is `EncodeJpeg` in
`Encoder.cpp`: `ComputeAllCoefficients` (setup and pixel phase), then `EncodeFromCoefficients`
(tokens, Huffman codes, `AssembleJpeg`). `ctJpeg --sequential` runs exactly this.

## Not from jpegli

* `Image.h`/`.cpp`: reading binary PPM/PGM with maxval 255, as cjpegli accepts them; the limit of
  65500 pixels per side is jpegli's.
* `SimdFeatures.h`/`.cpp`: the choice of the feature set (CPUID, XGETBV).
* `ByteBuffer.h`, `HeapArray.h`: memory on the process heap without the C++ standard containers.
* `src\Tasks\`, `src\App\`: the task graph and the command line; see
  [Reading the code](../README.md#reading-the-code).

[← back to the README](../README.md)
