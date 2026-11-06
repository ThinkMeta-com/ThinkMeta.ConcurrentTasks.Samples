//
// Copyright (c) 2026 ThinkMeta Software GmbH
// Licensed under the MIT License: LICENSE.txt in the ctDeflate folder of ThinkMeta.ConcurrentTasks.Samples.
//

#ifndef PCH_H
#define PCH_H

#if defined(_WIN32)
#    include <Windows.h>
#else
#    include <unistd.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "CppUnitTest.h"

#endif // PCH_H
