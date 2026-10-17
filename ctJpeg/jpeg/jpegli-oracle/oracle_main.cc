// cjpegli bit oracle with a selectable Highway target ("feature set").
// Links the unmodified jpegli library and the unmodified tools/cjpegli.cc (its main is renamed to
// CjpegliMain by a compile definition). Usage:
//   cjpegli-oracle --target <name> INPUT OUTPUT [cjpegli options]
//   cjpegli-oracle --list
// <name>: emu128, sse2, ssse3, sse4, avx2, avx3, avx3_dl, avx3_zen4, avx3_spr, avx10_2.
// The target must be compiled in and supported by the CPU, otherwise the oracle refuses to run
// instead of silently falling back to another target.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hwy/targets.h"

int CjpegliMain(int argc, const char** argv);

namespace {

struct TargetEntry {
    const char* name;
    int64_t bit;
};

const TargetEntry kTargets[] = {
    {"emu128", HWY_EMU128},   {"sse2", HWY_SSE2},       {"ssse3", HWY_SSSE3},
    {"sse4", HWY_SSE4},       {"avx2", HWY_AVX2},       {"avx3", HWY_AVX3},
    {"avx3_dl", HWY_AVX3_DL}, {"avx3_zen4", HWY_AVX3_ZEN4}, {"avx3_spr", HWY_AVX3_SPR},
    {"avx10_2", HWY_AVX10_2},
};

void ListTargets() {
    std::fprintf(stderr, "compiled and supported targets:");
    for (int64_t t : hwy::SupportedAndGeneratedTargets())
        std::fprintf(stderr, " %s", hwy::TargetName(t));
    std::fprintf(stderr, "\n");
}

} // namespace

int main(int argc, const char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
        ListTargets();
        return 0;
    }
    if (argc < 3 || std::strcmp(argv[1], "--target") != 0) {
        std::fprintf(stderr, "usage: %s --target <name> INPUT OUTPUT [cjpegli options] | --list\n", argv[0]);
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

    std::vector<const char*> args;
    args.push_back(argv[0]);
    for (int i = 3; i < argc; ++i)
        args.push_back(argv[i]);
    return CjpegliMain(static_cast<int>(args.size()), args.data());
}
