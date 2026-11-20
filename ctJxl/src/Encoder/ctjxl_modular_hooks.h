// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#ifndef CTJXL_MODULAR_HOOKS_H_
#define CTJXL_MODULAR_HOOKS_H_

// ctJxl: overloads with a thread pool for the learning of the global MA tree
// (plan D4.1). Defined in the copies of enc_ma.cc and enc_encoding.cc, called
// from the copy of enc_modular.cc; the libjxl headers stay unchanged, their
// functions call these with a null pool.

#include <cstdint>
#include <vector>

#include "lib/jxl/base/data_parallel.h"
#include "lib/jxl/base/status.h"
#include "lib/jxl/modular/encoding/dec_ma.h"
#include "lib/jxl/modular/encoding/enc_ma.h"
#include "lib/jxl/modular/modular_image.h"
#include "lib/jxl/modular/options.h"

namespace jxl {

Status ComputeBestTree(TreeSamples &tree_samples, float threshold,
                       const std::vector<ModularMultiplierInfo> &mul_info,
                       StaticPropRange static_prop_range,
                       float fast_decode_multiplier, Tree *tree,
                       ThreadPool *pool);

StatusOr<Tree> LearnTree(
    const Image *images, const ModularOptions *options, uint32_t start,
    uint32_t stop, const std::vector<ModularMultiplierInfo> &multiplier_info,
    ThreadPool *pool);

}  // namespace jxl

#endif  // CTJXL_MODULAR_HOOKS_H_
