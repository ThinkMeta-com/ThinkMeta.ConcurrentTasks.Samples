// cjxl bit oracle with a selectable Highway target ("feature set").
// Links the unmodified libjxl library and the unmodified tools/cjxl_main.cc (its main is renamed to
// CjxlMain by a compile definition). Usage:
//   cjxl-oracle --target <name> INPUT OUTPUT [cjxl options]
//   cjxl-oracle --list
// <name>: emu128, scalar, sse2, ssse3, sse4, avx2.
// The target must be compiled in and supported by the CPU, otherwise the oracle refuses to run
// instead of silently falling back to another target.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hwy/targets.h"

int CjxlMain(int argc, char** argv);

namespace {

struct TargetEntry {
    const char* name;
    int64_t bit;
};

const TargetEntry kTargets[] = {
    {"emu128", HWY_EMU128}, {"scalar", HWY_SCALAR}, {"sse2", HWY_SSE2},
    {"ssse3", HWY_SSSE3},   {"sse4", HWY_SSE4},     {"avx2", HWY_AVX2},
};

void ListTargets() {
    std::fprintf(stderr, "compiled and supported targets:");
    for (int64_t t : hwy::SupportedAndGeneratedTargets())
        std::fprintf(stderr, " %s", hwy::TargetName(t));
    std::fprintf(stderr, "\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
        ListTargets();
        return 0;
    }
    if (argc < 3 || std::strcmp(argv[1], "--target") != 0) {
        std::fprintf(stderr, "usage: %s --target <name> INPUT OUTPUT [cjxl options] | --list\n", argv[0]);
        return 1;
    }
    int64_t bit = 0;
    for (const auto& t : kTargets)
        if (std::strcmp(argv[2], t.name) == 0)
            bit = t.bit;
    if (bit == 0) {
        std::fprintf(stderr, "unknown target '%s'\n", argv[2]);
        return 1;
    }
    bool available = false;
    for (int64_t t : hwy::SupportedAndGeneratedTargets())
        available |= t == bit;
    if (!available) {
        std::fprintf(stderr, "target '%s' is not compiled in or not supported by this CPU\n", argv[2]);
        ListTargets();
        return 1;
    }
    hwy::SetSupportedTargetsForTest(bit);

    std::vector<char*> args;
    args.push_back(argv[0]);
    for (int i = 3; i < argc; ++i)
        args.push_back(argv[i]);
    args.push_back(nullptr);
    return CjxlMain(static_cast<int>(args.size() - 1), args.data());
}
