# From libdeflate to ctDeflate

ctDeflate's engine (`src\Encoder\`) is a hand-written C++ port of the compressor of libdeflate 1.26 (`libdeflate\libdeflate-1.26\lib\`), bit for bit the same output. This page maps the original functions to the port. In the code, each ported function carries the same reference above it, for example `// libdeflate: deflate_flush_block() (deflate_compress.c)`.

## Naming

* **Ported functions and structures** get C++ names that keep the original's words: `deflate_make_huffman_code()` → `MakeHuffmanCode`, `struct hc_matchfinder` → `HcMatchfinder`. The comment above names the original.
* **"follows libdeflate"** marks code that does the same as the original but is cut differently, usually so a speculative parse can stop and resume or be replayed (`Parser`, `BlockTracker`, `OrderedMatchfinder`).
* **"New in ctDeflate, not in libdeflate"** marks the parallel algorithm: chunks, parse paths, the resolver. libdeflate has nothing to compare these with.

## The sequential compressor

| libdeflate (file) | ctDeflate (file: name) |
|---|---|
| struct libdeflate_compressor, libdeflate_alloc_compressor(), libdeflate_alloc_compressor_ex() (deflate_compress.c) | Compressor.h/.cpp: `Compressor`, `Compressor::Create` |
| libdeflate_deflate_compress(), libdeflate_gzip_compress(), libdeflate_zlib_compress() | Compressor.cpp: `CompressDeflate`, `CompressGzip`, `CompressZlib` |
| libdeflate_deflate_compress_bound(), libdeflate_gzip_compress_bound(), libdeflate_zlib_compress_bound() | Compressor.h/.cpp: `DeflateBound`, `GzipBound`, `ZlibBound` |
| the gzip header and trailer of libdeflate_gzip_compress() (gzip_compress.c) | Gzip.h: `WriteGzipHeader`, `WriteGzipTrailer` |
| deflate_compress_none() | Compressor.cpp: `CompressNone` |
| deflate_compress_fastest(), deflate_compress_greedy(), deflate_compress_lazy_generic() | Compressor.cpp: `CompressFastest`, `CompressGreedy`, `CompressLazy` |
| deflate_begin_sequences(), deflate_reset_symbol_frequencies() | Compressor.cpp: `BeginSequences` |
| deflate_choose_literal(), deflate_choose_match(), observe_literal(), observe_match() | Compressor.cpp: `ChooseLiteral`, `ChooseMatch` |
| should_end_block(), recalculate_min_match_len() | Compressor.cpp: `ShouldEndBlock`, `RecalculateMinMatchLen` |
| adjust_max_and_nice_len() | Compressor.cpp: `AdjustMaxAndNiceLen` |
| the level table of libdeflate_alloc_compressor_ex() | BlockRules.h: `LevelParams::For` |
| struct block_split_stats, merge_new_observations(), do_end_block_check(), ready_to_check_block() | BlockRules.h: `SplitStats`, `MergeNewObservations`, `DoEndBlockCheck`, `EndBlockCheckDue` |
| choose_min_match_len(), calculate_min_match_len(), recalculate_min_match_len(), choose_max_block_end() | BlockRules.h: `ChooseMinMatchLen`, `CalculateMinMatchLen`, `RecalculateMinMatchLen`, `ChooseMaxBlockEnd` |
| bsr32() (common_defs.h) | BlockRules.h: `Bsr32` |

## Matchfinders

| libdeflate (file) | ctDeflate (file: name) |
|---|---|
| mf_pos_t, MATCHFINDER_WINDOW_SIZE, MATCHFINDER_INITVAL, matchfinder_init(), matchfinder_rebase() (matchfinder_common.h) | Matchfinder.h: `MfPos`, `MatchfinderInit`, `MatchfinderRebase` |
| lz_hash(), lz_extend() (matchfinder_common.h) | Matchfinder.h: `LzHash`, `LzExtend` |
| load_u32_unaligned(), load_word_unaligned(), prefetchw() (common_defs.h) | Matchfinder.h: `LoadU32`, `LoadU64`, `PrefetchW` |
| struct hc_matchfinder and its init, slide_window, longest_match, skip_bytes (hc_matchfinder.h) | Matchfinder.h: `HcMatchfinder::Init`, `SlideWindow`, `LongestMatch`, `SkipBytes` |
| struct ht_matchfinder and its init, slide_window, longest_match, skip_bytes (ht_matchfinder.h) | Matchfinder.h: `HtMatchfinder::Init`, `SlideWindow`, `LongestMatch`, `SkipBytes` |

## Blocks and Huffman codes

| libdeflate (deflate_compress.c) | ctDeflate (file: name) |
|---|---|
| struct deflate_output_bitstream, ADD_BITS(), FLUSH_BITS() | BitWriter.h: `BitWriter::AddBits`, `Flush`, `AlignToByte` |
| struct deflate_freqs, struct deflate_codes, struct deflate_sequence, SEQ_LENGTH_SHIFT | BlockWriter.h: `DeflateFreqs`, `DeflateCodes`, `DeflateSequence`, `SeqLengthShift` |
| deflate_init_static_codes() | BlockWriter.cpp: the `BlockWriter` constructor |
| deflate_finish_block() | BlockWriter.cpp: `BlockWriter::FinishBlock` |
| deflate_flush_block(): costs, block type, stored, Huffman block with WRITE_MATCH() | BlockWriter.cpp: `ComputeCosts`, `FlushBlock`, `WriteStored`, `WriteCompressed` |
| deflate_make_huffman_codes(), deflate_compute_precode_items(), deflate_precompute_huffman_header(), deflate_compute_full_len_codewords() | BlockWriter.cpp: `MakeHuffmanCodes`, `ComputePrecodeItems`, `PrecomputeHuffmanHeader`, `ComputeFullLenCodewords` |
| deflate_make_huffman_code(), heapify_subtree(), heap_sort(), sort_symbols(), build_tree(), compute_length_counts(), gen_codewords(), reverse_codeword() | Huffman.cpp: `MakeHuffmanCode`, `HeapifySubtree`, `HeapSort`, `SortSymbols`, `BuildTree`, `ComputeLengthCounts`, `GenCodewords`, `ReverseCodeword` |
| deflate_length_slot_base[], deflate_extra_length_bits[], deflate_offset_slot_base[], deflate_extra_offset_bits[], deflate_length_slot[], deflate_get_offset_slot(), deflate_precode_lens_permutation[], deflate_extra_precode_bits[] | DeflateTables.h: `LengthSlotBase`, `ExtraLengthBits`, `OffsetSlotBase`, `ExtraOffsetBits`, `LengthSlot`, `OffsetSlot`, `PrecodeLensPermutation`, `ExtraPrecodeBits` |

## Checksums

| Original | ctDeflate (file: name) |
|---|---|
| libdeflate_adler32(), adler32_generic() (adler32.c) | Checksums.cpp: `Adler32` |
| libdeflate_crc32() (crc32.c): same result, own implementation (PCLMUL folding, slice-by-8), not ported | Checksums.cpp: `Crc32` |
| zlib's crc32_combine() (not in libdeflate) | Checksums.cpp: `Crc32Combine` |

## Cut differently ("follows libdeflate")

| libdeflate (deflate_compress.c) | ctDeflate (file: name) |
|---|---|
| the block loops of deflate_compress_fastest() / _greedy() / _lazy_generic(), one iteration each | Parser.cpp: `Parser::Run`, `IterationFastest`, `IterationGreedy`, `IterationLazy` |
| the block start, the block loop condition, the min_len recalculation, observe_literal(), observe_match(), should_end_block() | BlockTracker.h/.cpp: `BeginBlock`, `ContinueBlock`, `PeekMinLen`, `Literal`/`Literals`/`LiteralObservations`, `Match`, `ShouldEndBlock` |
| hc_matchfinder_skip_bytes(), _longest_match(), _slide_window() | OrderedMatchfinder.h: `InsertUpTo`, `LongestMatch`, `Slide` (one matchfinder for several parse paths) |
| deflate_compress_greedy() / deflate_compress_lazy_generic() per group of parse paths | MultiParser.cpp: `MultiParser::Run` |
| deflate_begin_sequences(), deflate_choose_literal(), deflate_choose_match() | Resolver.cpp: `BuildBlockSymbols` |
| deflate_finish_block(), split into building and writing | BlockWriter.cpp: `BlockWriter::Encode` (the parallel encoding of a block) |

## New in ctDeflate

`ChunkedDeflate` (the chunks and the reference pipeline `CompressGzipChunked`), `MultiParser` (one speculative parse for several minimum match lengths), `ParseStep` (the record of a speculative parse), `Resolver` (joins the chunk parses into exactly the sequential result) and `EncodedBlock`. The task graph around them is in `src\Tasks\` and described in [task-graph.md](task-graph.md).
