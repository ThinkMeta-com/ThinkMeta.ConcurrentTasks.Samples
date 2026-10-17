// Copyright (c) 2026 ThinkMeta Software GmbH
// SPDX-License-Identifier: MIT (see LICENSE.txt)

// Deterministic synthetic test images for ctJpeg profiling / bit-compare (PPM P6, 8-bit RGB).
// Built with src\ctJpeg.slnx; verify\Prepare-Inputs.ps1 runs it.
// Usage: GenTestImages <outdir>
//   photo-6000x4000.ppm      multi-octave value noise "landscape", sky gradient, sensor grain (24 MP)
//   screenshot-3840x2160.ppm flat UI panels, borders, icons, pseudo-text glyphs (8 MP)
//   text-4960x7016.ppm       black pseudo-text on white, A4 @ 600 dpi (35 MP)
//        GenTestImages --edge <outdir>      small odd-sized photo-like images (1x1 ... 1001x777)
//        GenTestImages --gray <in.ppm> <out.pgm>   BT.601 luma of a P6 file as P5

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Rng {
    std::uint64_t s;
    std::uint64_t Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double Unit() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
    int Range(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<std::uint64_t>(hi - lo + 1)); }
};

struct Image {
    int w, h;
    std::vector<std::uint8_t> px;
    Image(int w_, int h_) : w(w_), h(h_), px(static_cast<std::size_t>(w_) * h_ * 3) {}
    void Set(int x, int y, int r, int g, int b) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        auto* p = &px[(static_cast<std::size_t>(y) * w + x) * 3];
        p[0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
        p[1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
        p[2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
    }
    void Fill(int x0, int y0, int x1, int y1, int r, int g, int b) {
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(w, x1); ++x)
                Set(x, y, r, g, b);
    }
    bool Save(const std::string& path) const {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
            return false;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
        return true;
    }
};

// Lattice value noise with smoothstep interpolation.
struct ValueNoise {
    static constexpr int N = 256;
    float lattice[N * N];
    explicit ValueNoise(std::uint64_t seed) {
        Rng r{seed};
        for (auto& v : lattice)
            v = static_cast<float>(r.Unit());
    }
    float At(int x, int y) const { return lattice[(y & (N - 1)) * N + (x & (N - 1))]; }
    float Sample(double x, double y) const {
        const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
        const double fx = x - xi, fy = y - yi;
        const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
        const double a = At(xi, yi) + (At(xi + 1, yi) - At(xi, yi)) * sx;
        const double b = At(xi, yi + 1) + (At(xi + 1, yi + 1) - At(xi, yi + 1)) * sx;
        return static_cast<float>(a + (b - a) * sy);
    }
    float Fbm(double x, double y, int octaves) const {
        double sum = 0, amp = 0.5, freq = 1;
        for (int o = 0; o < octaves; ++o) {
            sum += amp * Sample(x * freq, y * freq);
            freq *= 2.03;
            amp *= 0.5;
        }
        return static_cast<float>(sum);
    }
};

Image MakePhoto(int w, int h) {
    Image img(w, h);
    ValueNoise terrain(0x1234), detail(0x5678), color(0x9abc);
    Rng grain{0xdeadbeef};
    for (int y = 0; y < h; ++y) {
        const double v = static_cast<double>(y) / h;
        for (int x = 0; x < w; ++x) {
            const double u = static_cast<double>(x) / w;
            // Horizon line from low-frequency noise.
            const double horizon = 0.35 + 0.15 * terrain.Fbm(u * 3, 0.5, 4);
            double r, g, b;
            if (v < horizon) {
                // Sky: vertical gradient plus soft clouds.
                const double c = std::max(0.0, terrain.Fbm(u * 6, v * 12, 6) - 0.45) * 2.2;
                r = 90 + 80 * v + 150 * c;
                g = 140 + 60 * v + 140 * c;
                b = 220 - 20 * v + 30 * c;
            } else {
                // Ground: detailed texture (grass/rock) with colour variation.
                const double d = detail.Fbm(u * 400, v * 260, 7);
                const double m = color.Fbm(u * 8, v * 8, 4);
                const double shade = 0.55 + 0.9 * (d - 0.25);
                r = (70 + 110 * m) * shade;
                g = (90 + 70 * (1 - m)) * shade;
                b = (40 + 40 * m) * shade;
            }
            const double n = (grain.Unit() - 0.5) * 10; // sensor grain
            img.Set(x, y, static_cast<int>(r + n), static_cast<int>(g + n), static_cast<int>(b + n));
        }
    }
    return img;
}

struct Glyphs {
    // 96 random 5x7 glyph bitmaps, roughly letter-like density.
    std::uint64_t bits[96];
    explicit Glyphs(std::uint64_t seed) {
        Rng r{seed};
        for (auto& g : bits) {
            std::uint64_t v = 0;
            for (int i = 0; i < 35; ++i)
                if (r.Unit() < 0.42)
                    v |= 1ull << i;
            g = v;
        }
    }
    void Draw(Image& img, int x0, int y0, int idx, int scale, int r, int g, int b) const {
        const auto v = bits[idx % 96];
        for (int gy = 0; gy < 7; ++gy)
            for (int gx = 0; gx < 5; ++gx)
                if (v & (1ull << (gy * 5 + gx)))
                    img.Fill(x0 + gx * scale, y0 + gy * scale, x0 + (gx + 1) * scale, y0 + (gy + 1) * scale, r, g, b);
    }
    // Writes a line of words; returns nothing, wraps at maxX.
    void Line(Image& img, Rng& rng, int x0, int y0, int maxX, int scale, int r, int g, int b) const {
        int x = x0;
        while (x < maxX - 6 * scale) {
            const int wordLen = rng.Range(2, 9);
            if (x + wordLen * 6 * scale > maxX)
                break;
            for (int i = 0; i < wordLen; ++i, x += 6 * scale)
                Draw(img, x, y0, rng.Range(0, 95), scale, r, g, b);
            x += 4 * scale;
        }
    }
};

Image MakeScreenshot(int w, int h) {
    Image img(w, h);
    Rng rng{0x5c5c};
    Glyphs glyphs(0x6c79);
    img.Fill(0, 0, w, h, 243, 243, 243);
    img.Fill(0, 0, w, 64, 32, 36, 44);       // title bar
    img.Fill(0, 64, 420, h, 225, 228, 232);  // sidebar
    img.Fill(0, h - 48, w, h, 28, 30, 36);   // task bar
    for (int i = 0; i < 18; ++i) {            // sidebar entries with icons
        const int y = 100 + i * 56;
        img.Fill(24, y, 56, y + 32, 40 + i * 9, 120, 200 - i * 7);
        glyphs.Line(img, rng, 72, y + 6, 400, 3, 40, 40, 40);
    }
    // Content panels: editor-like text with syntax colours, cards, a gradient chart.
    img.Fill(460, 100, 2400, 2060, 255, 255, 255);
    for (int y = 120; y < 2040; y += 30) {
        const int c = rng.Range(0, 3);
        const int cols[4][3] = {{30, 30, 30}, {0, 90, 200}, {170, 40, 120}, {20, 130, 60}};
        glyphs.Line(img, rng, 480 + rng.Range(0, 6) * 24, y, 2380, 3, cols[c][0], cols[c][1], cols[c][2]);
    }
    for (int i = 0; i < 4; ++i) {
        const int y = 100 + i * 490;
        img.Fill(2440, y, 3800, y + 450, 255, 255, 255);
        img.Fill(2440, y, 3800, y + 4, 0, 120, 215);
        for (int l = 0; l < 6; ++l)
            glyphs.Line(img, rng, 2470, y + 30 + l * 40, 3770, 4, 50, 50, 50);
        for (int x = 2470; x < 3770; ++x) { // smooth area chart
            const double t = (x - 2470) / 1300.0;
            const int top = y + 440 - static_cast<int>(120 + 80 * std::sin(t * 9 + i) + 40 * std::sin(t * 31));
            for (int yy = top; yy < y + 440; ++yy) {
                const double f = static_cast<double>(yy - top) / (y + 440 - top + 1);
                img.Set(x, yy, static_cast<int>(0 + 200 * f), static_cast<int>(120 + 120 * f), 215);
            }
        }
    }
    return img;
}

Image MakeText(int w, int h) {
    Image img(w, h);
    Rng rng{0x7e47};
    Glyphs glyphs(0x6c79);
    img.Fill(0, 0, w, h, 255, 255, 255);
    int y = 500;
    glyphs.Line(img, rng, 600, y, 3600, 12, 0, 0, 0); // heading
    y += 200;
    while (y < h - 500) {
        for (int l = 0; l < rng.Range(6, 14) && y < h - 500; ++l, y += 100)
            glyphs.Line(img, rng, 600, y, w - 600, 9, 0, 0, 0);
        y += 100; // paragraph gap
    }
    return img;
}

} // namespace

// Small photo-like images with odd sizes for edge handling (partial MCUs, 1-pixel images).
Image MakeEdge(int w, int h) {
    Image img(w, h);
    ValueNoise n(0x0dd5 + static_cast<std::uint64_t>(w) * 131 + h);
    Rng grain{0x1234u + static_cast<std::uint64_t>(w) * h};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double u = static_cast<double>(x) / 37, v = static_cast<double>(y) / 37;
            const double a = n.Fbm(u, v, 5), b = n.Fbm(u + 50, v, 5), c = n.Fbm(u, v + 50, 5);
            const double g = (grain.Unit() - 0.5) * 12;
            img.Set(x, y, static_cast<int>(255 * a + g), static_cast<int>(255 * b + g), static_cast<int>(255 * c + g));
        }
    return img;
}

// Writes the BT.601 luma of a P6 file as P5 (integer, rounded) - grayscale inputs from the photos.
int ToGray(const char* in, const char* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, in, "rb") != 0 || !f)
        return 1;
    int w = 0, h = 0, maxval = 0;
    if (fscanf_s(f, "P6 %d %d %d", &w, &h, &maxval) != 3 || maxval != 255) {
        std::fclose(f);
        return 1;
    }
    std::fgetc(f); // single whitespace after maxval
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3), gray(static_cast<std::size_t>(w) * h);
    const bool ok = std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size();
    std::fclose(f);
    if (!ok)
        return 1;
    for (std::size_t i = 0; i < gray.size(); ++i)
        gray[i] = static_cast<std::uint8_t>((299 * rgb[3 * i] + 587 * rgb[3 * i + 1] + 114 * rgb[3 * i + 2] + 500) / 1000);
    if (fopen_s(&f, out, "wb") != 0 || !f)
        return 1;
    std::fprintf(f, "P5\n%d %d\n255\n", w, h);
    std::fwrite(gray.data(), 1, gray.size(), f);
    std::fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--gray")
        return ToGray(argv[2], argv[3]);
    if (argc == 3 && std::string(argv[1]) == "--edge") {
        const std::string edgeDir = std::string(argv[2]) + "\\";
        const int sizes[][2] = {{1, 1}, {7, 5}, {8, 8}, {17, 9}, {16, 16}, {33, 31}, {257, 129}, {1001, 777}};
        for (const auto& s : sizes) {
            const std::string name = "edge-" + std::to_string(s[0]) + "x" + std::to_string(s[1]) + ".ppm";
            if (!MakeEdge(s[0], s[1]).Save(edgeDir + name))
                return 1;
            std::printf("%s\n", name.c_str());
        }
        return 0;
    }
    const std::string dir = argc > 1 ? std::string(argv[1]) + "\\" : std::string();
    struct Job {
        const char* name;
        Image (*make)(int, int);
        int w, h;
    } jobs[] = {
        {"photo-6000x4000.ppm", MakePhoto, 6000, 4000},
        {"screenshot-3840x2160.ppm", MakeScreenshot, 3840, 2160},
        {"text-4960x7016.ppm", MakeText, 4960, 7016},
    };
    for (const auto& j : jobs) {
        const auto img = j.make(j.w, j.h);
        if (!img.Save(dir + j.name)) {
            std::fprintf(stderr, "cannot write %s\n", j.name);
            return 1;
        }
        std::printf("%s\n", j.name);
    }
    return 0;
}
