// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#pragma once

// The few compiler-specific words of the samples, for MSVC (Windows) and clang (Linux). Preliminary.

/// <summary>Inlines a function in every call (MSVC __forceinline).</summary>
#if defined(_MSC_VER)
#    define CT_FORCEINLINE __forceinline
#else
#    define CT_FORCEINLINE inline __attribute__((always_inline))
#endif
