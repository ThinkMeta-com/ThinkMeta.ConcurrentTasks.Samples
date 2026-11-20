// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#ifndef CTJXL_APNG_H_
#define CTJXL_APNG_H_

// ctJxl: a PNG input whose rows are decoded while the encoder reads them (plan D1.6b). The decoder is a marked copy
// of libjxl's lib/extras/dec/apng.cc (ctjxl_apng.cc), so that the image, its metadata and its frame header are the
// same as with DecodeBytes.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "lib/extras/dec/color_hints.h"
#include "lib/extras/packed_image.h"
#include "lib/jxl/base/span.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Jxl {
class InputBands;
}

namespace jxl {
namespace extras {
namespace ctjxl_apng {

class ChunkedPngInput {
 public:
  // The decoder publishes the rows in bands of this height.
  static constexpr size_t kRowsPerBand = 64;

  ChunkedPngInput();
  ~ChunkedPngInput();
  ChunkedPngInput(const ChunkedPngInput&) = delete;
  ChunkedPngInput& operator=(const ChunkedPngInput&) = delete;

  // Checks the chunk list (one frame, not interlaced, no chunk but IDAT between
  // the first IDAT and IEND) and reads everything before the first IDAT: ppf
  // gets the info, color encoding, metadata and frame header of DecodeBytes and
  // one chunked frame whose rows come from DecodeRows. False: not suitable or
  // an error; the caller decodes the usual way. bytes must live until the
  // encoding has ended.
  bool ReadHeader(Span<const uint8_t> bytes, const ColorHints& color_hints,
                  PackedPixelFile* ppf);

  // The rows of the chunked frame wait for these bands (null: DecodeRows has
  // run before the encoding).
  void SetBands(ThinkMeta::ConcurrentTasks::Samples::Jxl::InputBands* bands);

  // Decodes the rows from the first IDAT on and publishes them to the bands.
  // False on a decoding error (as DecodeBytes would fail).
  bool DecodeRows();

  // DecodeRows as the read function of InputBands::StartReader.
  static bool DecodeRowsTask(void* self);

  struct Impl;  // in ctjxl_apng.cc, next to the decoder it shares

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace ctjxl_apng
}  // namespace extras
}  // namespace jxl

#endif  // CTJXL_APNG_H_
