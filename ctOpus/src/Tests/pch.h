// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

#ifndef PCH_H
#define PCH_H

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <unistd.h>
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "CppUnitTest.h"

#endif // PCH_H
