// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "BitCount.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "SessionConfig.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    // constant tables taken over from LAME (takehiro.c, quantize_pvt.c, tables.c) as data
    const std::array<std::int32_t, SBMAX_l> pretab    = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0};
    const std::array<std::int32_t, 16>      slen1_tab = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
    const std::array<std::int32_t, 16>      slen2_tab = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};

    namespace {

        // constant table taken over from LAME (takehiro.c) as data:
        // region0_count and region1_count by the number of scalefactor bands with big values
        const struct {
            std::int32_t region0_count;
            std::int32_t region1_count;
        } subdv_table[23] = {
            {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 1}, {1, 2}, {2, 2}, {2, 3}, {2, 3},
            {3, 4}, {3, 4}, {3, 4}, {4, 5}, {4, 5}, {4, 6}, {5, 6}, {5, 6}, {5, 7}, {6, 7}, {6, 7},
        };

        // limits of slen1 and slen2 by scalefac_compress
        const std::array<std::int32_t, 16> slen1_n = {1, 1, 1, 1, 8, 2, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16};
        const std::array<std::int32_t, 16> slen2_n = {1, 2, 4, 8, 1, 2, 4, 8, 2, 4, 8, 2, 4, 8, 4, 8};

        // bits for the scalefactors by scalefac_compress:
        // short 18 * slen1 + 18 * slen2, mixed 17 * slen1 + 18 * slen2, long 11 * slen1 + 10 * slen2
        const std::array<std::int32_t, 16> scale_short = {0, 18, 36, 54, 54, 36, 54, 72, 54, 72, 90, 72, 90, 108, 108, 126};
        const std::array<std::int32_t, 16> scale_mixed = {0, 18, 36, 54, 51, 35, 53, 71, 52, 70, 88, 69, 87, 105, 104, 122};
        const std::array<std::int32_t, 16> scale_long  = {0, 10, 20, 30, 33, 21, 31, 41, 32, 42, 52, 43, 53, 63, 64, 74};

        // first table without escape words for a maximum value of 1..15
        const std::array<std::int32_t, 15> huf_tbl_noESC = {1, 2, 5, 7, 7, 10, 10, 13, 13, 13, 13, 13, 13, 13, 13};

        // the scfsi bands of 2.4.2.7 of the IS
        const std::array<std::int32_t, 5> scfsi_band = {0, 6, 11, 16, 21};

        // largest value of pairs, scanned like LAME's ix_max: two interleaved maxima
        std::int32_t MaxValue(const std::int32_t* ix, const std::int32_t* end) noexcept
        {
            std::int32_t even = 0;
            std::int32_t odd  = 0;
            do {
                const std::int32_t a = *ix++;
                const std::int32_t b = *ix++;
                if (even < a)
                    even = a;
                if (odd < b)
                    odd = b;
            } while (ix < end);
            return even < odd ? odd : even;
        }

        // all values 0 or 1: table 1
        std::int32_t CountTable1(const std::int32_t* ix, const std::int32_t* end, std::uint32_t& bits) noexcept
        {
            const std::uint8_t* const hlen = ht[1].hlen;
            std::uint32_t             sum  = 0;
            do {
                const std::uint32_t x = static_cast<std::uint32_t>(ix[0]);
                const std::uint32_t y = static_cast<std::uint32_t>(ix[1]);
                sum += hlen[x + x + y];
                ix += 2;
            } while (ix < end);
            bits += sum;
            return 1;
        }

        // values up to 2 or 3: the pair tables 2/3 or 5/6 count two tables at once
        std::int32_t CountTablePair(const QuantizeConstants& constants, const std::int32_t* ix, const std::int32_t* end, std::int32_t max,
                                    std::uint32_t& bits) noexcept
        {
            std::int32_t         table = huf_tbl_noESC[max - 1];
            const std::uint32_t  xlen  = ht[table].xlen;
            const std::uint32_t* pair  = table == 2 ? constants.pairs.table23.data() : constants.pairs.table56.data();
            std::uint32_t        sum   = 0;
            do {
                const std::uint32_t x = static_cast<std::uint32_t>(ix[0]);
                const std::uint32_t y = static_cast<std::uint32_t>(ix[1]);
                sum += pair[x * xlen + y];
                ix += 2;
            } while (ix < end);

            const std::uint32_t second = sum & 0xffffu;
            std::uint32_t       first  = sum >> 16u;
            if (first > second) {
                first = second;
                ++table;
            }
            bits += first;
            return table;
        }

        // values up to 15: three neighbouring tables counted in one pass
        std::int32_t CountTableTriple(const std::int32_t* ix, const std::int32_t* end, std::int32_t max, std::uint32_t& bits) noexcept
        {
            const std::int32_t  table = huf_tbl_noESC[max - 1];
            const std::uint32_t xlen  = ht[table].xlen;
            const std::uint8_t* h0    = ht[table].hlen;
            const std::uint8_t* h1    = ht[table + 1].hlen;
            const std::uint8_t* h2    = ht[table + 2].hlen;
            std::uint32_t       sum0  = 0;
            std::uint32_t       sum1  = 0;
            std::uint32_t       sum2  = 0;
            do {
                const std::uint32_t index = static_cast<std::uint32_t>(ix[0]) * xlen + static_cast<std::uint32_t>(ix[1]);
                sum0 += h0[index];
                sum1 += h1[index];
                sum2 += h2[index];
                ix += 2;
            } while (ix < end);

            std::int32_t best = table;
            if (sum0 > sum1) {
                sum0 = sum1;
                ++best;
            }
            if (sum0 > sum2) {
                sum0 = sum2;
                best = table + 2;
            }
            bits += sum0;
            return best;
        }

        // values above 15: two tables with linbits (16..23 and 24..31) counted at once
        std::int32_t CountTableEscape(const QuantizeConstants& constants, const std::int32_t* ix, const std::int32_t* end, std::int32_t table1,
                                      std::int32_t table2, std::uint32_t& bits) noexcept
        {
            const std::uint32_t linbits = ht[table1].xlen * 65536u + ht[table2].xlen;
            std::uint32_t       sum     = 0;
            do {
                std::uint32_t x = static_cast<std::uint32_t>(ix[0]);
                std::uint32_t y = static_cast<std::uint32_t>(ix[1]);
                ix += 2;
                if (x >= 15u) {
                    x = 15u;
                    sum += linbits;
                }
                if (y >= 15u) {
                    y = 15u;
                    sum += linbits;
                }
                sum += constants.pairs.largetbl[(x << 4u) + y];
            } while (ix < end);

            const std::uint32_t second = sum & 0xffffu;
            std::uint32_t       first  = sum >> 16u;
            if (first > second) {
                first  = second;
                table1 = table2;
            }
            bits += first;
            return table1;
        }

        // choose_table: the Huffman table that codes ix[0..end) with the fewest bits; adds the
        // bits to *bits (LARGE_BITS replaces them if a value is too large)
        std::int32_t ChooseTable(const QuantizeConstants& constants, const std::int32_t* ix, const std::int32_t* end, std::int32_t* bits) noexcept
        {
            std::uint32_t& sum = *reinterpret_cast<std::uint32_t*>(bits);
            std::uint32_t  max = static_cast<std::uint32_t>(MaxValue(ix, end));

            if (max <= 15) {
                switch (max) {
                    case 0: return 0;
                    case 1: return CountTable1(ix, end, sum);
                    case 2:
                    case 3: return CountTablePair(constants, ix, end, static_cast<std::int32_t>(max), sum);
                    default: return CountTableTriple(ix, end, static_cast<std::int32_t>(max), sum);
                }
            }
            if (max > IXMAX_VAL) {
                *bits = LARGE_BITS;
                return -1;
            }

            // the smallest tables with enough linbits in both groups
            max -= 15u;
            std::int32_t escape24 = 24;
            while (escape24 < 32 && ht[escape24].linmax < max)
                ++escape24;
            std::int32_t escape16 = escape24 - 8;
            while (escape16 < 24 && ht[escape16].linmax < max)
                ++escape16;
            return CountTableEscape(constants, ix, end, escape16, escape24, sum);
        }

        // count1 region: quadruples of values 0/1 with table A (t32l) and B (t33l)
        std::int32_t QuadrupleIndex(const std::int32_t* ix, std::int32_t i) noexcept
        {
            return ((ix[i - 4] * 2 + ix[i - 3]) * 2 + ix[i - 2]) * 2 + ix[i - 1];
        }

        // best_huffman_divide asks choose_table for many ranges of big values, all starting at a
        // scalefactor band boundary and ending at one or at big_values; choose_table scans each
        // range again. BandCounts evaluates each band once instead and answers a range from
        // prefix sums over the bands. All sums are integers (modulo 2^32, also the two 16-bit
        // halves of the packed tables), so differences of prefix sums are exactly the sums
        // choose_table adds pair by pair, and the choice follows the same comparisons.
        class BandCounts
        {
        public:
            // segment k is band k, [bands[k], bands[k + 1]), the last one ends at big_values
            void Init(const QuantizeConstants& constants, const std::int32_t* ix, const std::int32_t* bands, std::int32_t bigValues) noexcept
            {
                constants_ = &constants;
                ix_        = ix;
                bands_     = bands;
                count_     = 0;
                SetEnd(bigValues);
            }

            // a new end (big_values after moving a pair into the count1 region): the full bands
            // before it keep their sums
            void SetEnd(std::int32_t bigValues) noexcept
            {
                std::int32_t last = 0;
                while (bands_[last + 1] < bigValues)
                    ++last;
                for (std::int32_t k = 0; k <= last; ++k) {
                    const std::int32_t begin = bands_[k];
                    const std::int32_t end   = k < last ? bands_[k + 1] : bigValues;
                    Segment&           seg   = segments_[k];
                    if (k < count_ && seg.begin == begin && seg.end == end)
                        continue;
                    seg.begin = begin;
                    seg.end   = end;
                    seg.max   = MaxValue(ix_ + begin, ix_ + end);
                    seg.valid = 0;
                }
                count_ = last + 1;

                // the maximum of every range of segments
                for (std::int32_t first = 0; first < count_; ++first) {
                    std::int32_t max = 0;
                    for (std::int32_t k = first; k < count_; ++k) {
                        if (max < segments_[k].max)
                            max = segments_[k].max;
                        rangeMax_[first][k + 1] = max;
                    }
                }
                prefixValid_ = 0;
            }

            std::int32_t Segments() const noexcept { return count_; }

            // choose_table for the segments [first, last)
            std::int32_t Choose(std::int32_t first, std::int32_t last, std::int32_t* bits) noexcept
            {
                std::uint32_t& sum = *reinterpret_cast<std::uint32_t*>(bits);
                std::uint32_t  max = static_cast<std::uint32_t>(rangeMax_[first][last]);

                if (max <= 15) {
                    switch (max) {
                        case 0: return 0;
                        case 1: {
                            sum += Sum(first, last, Class1, 0);
                            return 1;
                        }
                        case 2:
                        case 3: {
                            std::int32_t        table  = huf_tbl_noESC[max - 1];
                            const std::uint32_t packed = Sum(first, last, table == 2 ? Class23 : Class56, 0);
                            const std::uint32_t second = packed & 0xffffu;
                            std::uint32_t       best   = packed >> 16u;
                            if (best > second) {
                                best = second;
                                ++table;
                            }
                            sum += best;
                            return table;
                        }
                        default: {
                            const std::int32_t  table = huf_tbl_noESC[max - 1];
                            const CountClass    cls   = table == 7 ? Class7 : table == 10 ? Class10 : Class13;
                            std::uint32_t       sum0  = Sum(first, last, cls, 0);
                            const std::uint32_t sum1  = Sum(first, last, cls, 1);
                            const std::uint32_t sum2  = Sum(first, last, cls, 2);
                            std::int32_t        best  = table;
                            if (sum0 > sum1) {
                                sum0 = sum1;
                                ++best;
                            }
                            if (sum0 > sum2) {
                                sum0 = sum2;
                                best = table + 2;
                            }
                            sum += sum0;
                            return best;
                        }
                    }
                }
                if (max > IXMAX_VAL) {
                    *bits = LARGE_BITS;
                    return -1;
                }

                max -= 15u;
                std::int32_t escape24 = 24;
                while (escape24 < 32 && ht[escape24].linmax < max)
                    ++escape24;
                std::int32_t escape16 = escape24 - 8;
                while (escape16 < 24 && ht[escape16].linmax < max)
                    ++escape16;
                const std::uint32_t linbits = ht[escape16].xlen * 65536u + ht[escape24].xlen;
                const std::uint32_t packed  = Sum(first, last, ClassEscape, 0) + Sum(first, last, ClassEscape, 1) * linbits;
                const std::uint32_t second  = packed & 0xffffu;
                std::uint32_t       best    = packed >> 16u;
                std::int32_t        table   = escape16;
                if (best > second) {
                    best  = second;
                    table = escape24;
                }
                sum += best;
                return table;
            }

        private:
            // the table groups choose_table counts in one pass; the escape group keeps the
            // packed lengths of tables 16/24 and, in [1], the number of values >= 15
            enum CountClass : std::int32_t { Class1, Class23, Class56, Class7, Class10, Class13, ClassEscape, ClassCount };

            // the largest value each group's tables can index (xlen - 1); escape clamps to 15
            static constexpr std::array<std::int32_t, ClassCount> classMax = {1, 2, 3, 5, 7, 15, 0x7fffffff};

            struct Segment {
                std::int32_t                                         begin;
                std::int32_t                                         end;
                std::int32_t                                         max;
                std::uint32_t                                        valid; // bit per class
                std::array<std::array<std::uint32_t, 3>, ClassCount> sum;
            };

            // the sum of a class over [first, last); every segment of the range fits the class
            std::uint32_t Sum(std::int32_t first, std::int32_t last, CountClass cls, std::int32_t i) noexcept
            {
                if ((prefixValid_ & (1u << cls)) == 0)
                    BuildPrefix(cls);
                return prefix_[cls][i][last] - prefix_[cls][i][first];
            }

            // prefix sums of a class; segments with values the class's tables cannot index count
            // as 0 (no range of this class contains them)
            void BuildPrefix(CountClass cls) noexcept
            {
                auto& prefix = prefix_[cls];
                prefix[0][0] = prefix[1][0] = prefix[2][0] = 0;
                for (std::int32_t k = 0; k < count_; ++k) {
                    Segment&   seg  = segments_[k];
                    const bool fits = seg.max <= classMax[cls];
                    if (fits && (seg.valid & (1u << cls)) == 0)
                        Count(seg, cls);
                    for (std::int32_t i = 0; i < 3; ++i)
                        prefix[i][k + 1] = prefix[i][k] + (fits ? seg.sum[cls][i] : 0u);
                }
                prefixValid_ |= 1u << cls;
            }

            // the sums of one class over a segment, as the count functions add them
            void Count(Segment& seg, CountClass cls) noexcept
            {
                const std::int32_t* ix  = ix_ + seg.begin;
                const std::int32_t* end = ix_ + seg.end;
                std::uint32_t*      out = seg.sum[cls].data();
                out[0] = out[1] = out[2] = 0;
                switch (cls) {
                    case Class1: {
                        const std::uint8_t* const hlen = ht[1].hlen;
                        for (; ix < end; ix += 2)
                            out[0] += hlen[static_cast<std::uint32_t>(ix[0]) * 2u + static_cast<std::uint32_t>(ix[1])];
                        break;
                    }
                    case Class23:
                    case Class56: {
                        const std::int32_t   table = cls == Class23 ? 2 : 5;
                        const std::uint32_t  xlen  = ht[table].xlen;
                        const std::uint32_t* pair  = cls == Class23 ? constants_->pairs.table23.data() : constants_->pairs.table56.data();
                        for (; ix < end; ix += 2)
                            out[0] += pair[static_cast<std::uint32_t>(ix[0]) * xlen + static_cast<std::uint32_t>(ix[1])];
                        break;
                    }
                    case Class7:
                    case Class10:
                    case Class13: {
                        const std::int32_t  table = cls == Class7 ? 7 : cls == Class10 ? 10 : 13;
                        const std::uint32_t xlen  = ht[table].xlen;
                        const std::uint8_t* h0    = ht[table].hlen;
                        const std::uint8_t* h1    = ht[table + 1].hlen;
                        const std::uint8_t* h2    = ht[table + 2].hlen;
                        for (; ix < end; ix += 2) {
                            const std::uint32_t index = static_cast<std::uint32_t>(ix[0]) * xlen + static_cast<std::uint32_t>(ix[1]);
                            out[0] += h0[index];
                            out[1] += h1[index];
                            out[2] += h2[index];
                        }
                        break;
                    }
                    default: {
                        for (; ix < end; ix += 2) {
                            std::uint32_t x = static_cast<std::uint32_t>(ix[0]);
                            std::uint32_t y = static_cast<std::uint32_t>(ix[1]);
                            if (x >= 15u) {
                                x = 15u;
                                ++out[1];
                            }
                            if (y >= 15u) {
                                y = 15u;
                                ++out[1];
                            }
                            out[0] += constants_->pairs.largetbl[(x << 4u) + y];
                        }
                        break;
                    }
                }
                seg.valid |= 1u << cls;
            }

            const QuantizeConstants*                                                      constants_   = nullptr;
            const std::int32_t*                                                           ix_          = nullptr;
            const std::int32_t*                                                           bands_       = nullptr;
            std::int32_t                                                                  count_       = 0;
            std::uint32_t                                                                 prefixValid_ = 0; // bit per class
            std::array<Segment, SBMAX_l + 1>                                              segments_;
            std::array<std::array<std::int32_t, SBMAX_l + 2>, SBMAX_l + 1>                rangeMax_;
            std::array<std::array<std::array<std::uint32_t, SBMAX_l + 2>, 3>, ClassCount> prefix_;
        };

        // per split of region0 and region1 (7 + 15 + 1 band boundaries)
        using SplitTable = std::array<std::int32_t, 7 + 15 + 1>;

        // recalc_divide_init: the best tables for region0 and region1 for every split
        void SplitInit(const SessionConfig& config, BandCounts& counts, const GrInfo& gi, SplitTable& r01Bits, SplitTable& r01Div, SplitTable& r0Table,
                       SplitTable& r1Table) noexcept
        {
            const std::int32_t  bigValues = gi.big_values;
            const std::int32_t* bands     = config.scalefac_band.l.data();

            for (std::int32_t r = 0; r <= 7 + 15; ++r)
                r01Bits[r] = LARGE_BITS;

            for (std::int32_t r0 = 0; r0 < 16; ++r0) {
                const std::int32_t a1 = bands[r0 + 1];
                if (a1 >= bigValues)
                    break;
                std::int32_t       r0Bits = 0;
                const std::int32_t r0t    = counts.Choose(0, r0 + 1, &r0Bits);

                for (std::int32_t r1 = 0; r1 < 8; ++r1) {
                    const std::int32_t a2 = bands[r0 + r1 + 2];
                    if (a2 >= bigValues)
                        break;
                    std::int32_t       bits = r0Bits;
                    const std::int32_t r1t  = counts.Choose(r0 + 1, r0 + r1 + 2, &bits);
                    if (r01Bits[r0 + r1] > bits) {
                        r01Bits[r0 + r1] = bits;
                        r01Div[r0 + r1]  = r0;
                        r0Table[r0 + r1] = r0t;
                        r1Table[r0 + r1] = r1t;
                    }
                }
            }
        }

        // recalc_divide_sub: tries every region2 start with the best region0/1 split; takes
        // candidate's data whenever it is cheaper than what gi has
        void SplitTry(const SessionConfig& config, BandCounts& counts, const GrInfo& candidate, GrInfo& gi, const SplitTable& r01Bits, const SplitTable& r01Div,
                      const SplitTable& r0Table, const SplitTable& r1Table) noexcept
        {
            const std::int32_t  bigValues = candidate.big_values;
            const std::int32_t* bands     = config.scalefac_band.l.data();

            for (std::int32_t r2 = 2; r2 < SBMAX_l + 1; ++r2) {
                const std::int32_t a2 = bands[r2];
                if (a2 >= bigValues)
                    break;

                std::int32_t bits = r01Bits[r2 - 2] + candidate.count1bits;
                if (gi.part2_3_length <= bits)
                    break;

                const std::int32_t r2t = counts.Choose(r2, counts.Segments(), &bits);
                if (gi.part2_3_length <= bits)
                    continue;

                std::memcpy(&gi, &candidate, sizeof(GrInfo));
                gi.part2_3_length  = bits;
                gi.region0_count   = r01Div[r2 - 2];
                gi.region1_count   = r2 - 2 - r01Div[r2 - 2];
                gi.table_select[0] = r0Table[r2 - 2];
                gi.table_select[1] = r1Table[r2 - 2];
                gi.table_select[2] = r2t;
            }
        }

        // scfsi_calc: shares the scalefactors of granule 1 with granule 0 where they are equal
        void ScfsiCalc(std::int32_t ch, SideInfo& side) noexcept
        {
            GrInfo&       gi = side.tt[1][ch];
            const GrInfo& g0 = side.tt[0][ch];

            for (std::int32_t i = 0; i < 4; ++i) {
                std::int32_t sfb = scfsi_band[i];
                for (; sfb < scfsi_band[i + 1]; ++sfb) {
                    if (g0.scalefac[sfb] != gi.scalefac[sfb] && gi.scalefac[sfb] >= 0)
                        break;
                }
                if (sfb == scfsi_band[i + 1]) {
                    for (sfb = scfsi_band[i]; sfb < scfsi_band[i + 1]; ++sfb)
                        gi.scalefac[sfb] = -1;
                    side.scfsi[ch][i] = 1;
                }
            }

            std::int32_t max1 = 0, count1 = 0;
            std::int32_t sfb = 0;
            for (; sfb < 11; ++sfb) {
                if (gi.scalefac[sfb] == -1)
                    continue;
                ++count1;
                if (max1 < gi.scalefac[sfb])
                    max1 = gi.scalefac[sfb];
            }
            std::int32_t max2 = 0, count2 = 0;
            for (; sfb < SBPSY_l; ++sfb) {
                if (gi.scalefac[sfb] == -1)
                    continue;
                ++count2;
                if (max2 < gi.scalefac[sfb])
                    max2 = gi.scalefac[sfb];
            }

            for (std::int32_t i = 0; i < 16; ++i) {
                if (max1 < slen1_n[i] && max2 < slen2_n[i]) {
                    const std::int32_t bits = slen1_tab[i] * count1 + slen2_tab[i] * count2;
                    if (gi.part2_length > bits) {
                        gi.part2_length      = bits;
                        gi.scalefac_compress = i;
                    }
                }
            }
        }

    } // namespace

    void InitRegionSplit(const SessionConfig& config, std::array<std::int32_t, 576>& bvScf) noexcept
    {
        const std::int32_t* bands = config.scalefac_band.l.data();
        for (std::int32_t i = 2; i <= 576; i += 2) {
            // number of bands up to i, then the largest region0 that does not pass i
            std::int32_t bandCount = 0;
            while (bands[++bandCount] < i) {
            }

            std::int32_t region0 = subdv_table[bandCount].region0_count;
            while (bands[region0 + 1] > i)
                --region0;
            if (region0 < 0) {
                // everything goes into region0 (big_values < region0 < region1), so region0 and
                // region1 get values larger than big_values
                region0 = subdv_table[bandCount].region0_count;
            }
            bvScf[i - 2] = region0;

            std::int32_t region1 = subdv_table[bandCount].region1_count;
            while (bands[region1 + bvScf[i - 2] + 2] > i)
                --region1;
            if (region1 < 0)
                region1 = subdv_table[bandCount].region1_count;
            bvScf[i - 1] = region1;
        }
    }

    std::int32_t CountBits(const SessionConfig& config, const QuantizeConstants& constants, GrInfo& gi) noexcept
    {
        const std::int32_t* const ix = gi.l3_enc.data();
        std::int32_t              i  = Min(576, ((gi.max_nonzero_coeff + 2) >> 1) << 1);

        // count1 region: from the last pair that is not zero
        for (; i > 1; i -= 2) {
            if (ix[i - 1] | ix[i - 2])
                break;
        }
        gi.count1 = i;

        // quadruples of 0/1 values, counted with both count1 tables
        std::int32_t bitsA = 0;
        std::int32_t bitsB = 0;
        for (; i > 3; i -= 4) {
            if (static_cast<std::uint32_t>(ix[i - 4] | ix[i - 3] | ix[i - 2] | ix[i - 1]) > 1)
                break;
            const std::int32_t p = QuadrupleIndex(ix, i);
            bitsA += t32l[p];
            bitsB += t33l[p];
        }
        std::int32_t bits     = bitsA;
        gi.count1table_select = 0;
        if (bitsA > bitsB) {
            bits                  = bitsB;
            gi.count1table_select = 1;
        }
        gi.count1bits = bits;
        gi.big_values = i;
        if (i == 0)
            return bits;

        // the regions of the big values
        std::int32_t a1, a2;
        if (gi.block_type == SHORT_TYPE) {
            a1 = 3 * config.scalefac_band.s[3];
            if (a1 > gi.big_values)
                a1 = gi.big_values;
            a2 = gi.big_values;
        } else if (gi.block_type == NORM_TYPE) {
            a1 = gi.region0_count = constants.bv_scf[i - 2];
            a2 = gi.region1_count = constants.bv_scf[i - 1];
            a2                    = config.scalefac_band.l[a1 + a2 + 2];
            a1                    = config.scalefac_band.l[a1 + 1];
            if (a2 < i)
                gi.table_select[2] = ChooseTable(constants, ix + a2, ix + i, &bits);
        } else {
            gi.region0_count = 7;
            gi.region1_count = SBMAX_l - 1 - 7 - 1;
            a1               = config.scalefac_band.l[7 + 1];
            a2               = i;
            if (a1 > a2)
                a1 = a2;
        }

        // big_values may end before region0 or region1
        a1 = Min(a1, i);
        a2 = Min(a2, i);

        if (0 < a1)
            gi.table_select[0] = ChooseTable(constants, ix, ix + a1, &bits);
        if (a1 < a2)
            gi.table_select[1] = ChooseTable(constants, ix + a1, ix + a2, &bits);
        if (config.use_best_huffman == 2) {
            gi.part2_3_length = bits;
            BestHuffmanDivide(config, constants, gi);
            bits = gi.part2_3_length;
        }
        return bits;
    }

    void BestHuffmanDivide(const SessionConfig& config, const QuantizeConstants& constants, GrInfo& gi) noexcept
    {
        const std::int32_t* const ix = gi.l3_enc.data();
        GrInfo                    candidate;
        SplitTable                r01Bits;
        SplitTable                r01Div;
        SplitTable                r0Table;
        SplitTable                r1Table;

        BandCounts counts;

        std::memcpy(&candidate, &gi, sizeof(GrInfo));
        if (gi.block_type == NORM_TYPE) {
            counts.Init(constants, ix, config.scalefac_band.l.data(), gi.big_values);
            SplitInit(config, counts, gi, r01Bits, r01Div, r0Table, r1Table);
            SplitTry(config, counts, candidate, gi, r01Bits, r01Div, r0Table, r1Table);
        }

        // can the last pair of big values move into the count1 region?
        std::int32_t i = candidate.big_values;
        if (i == 0 || static_cast<std::uint32_t>(ix[i - 2] | ix[i - 1]) > 1)
            return;
        i = gi.count1 + 2;
        if (i > 576)
            return;

        std::memcpy(&candidate, &gi, sizeof(GrInfo));
        candidate.count1   = i;
        std::int32_t bitsA = 0;
        std::int32_t bitsB = 0;
        for (; i > candidate.big_values; i -= 4) {
            const std::int32_t p = QuadrupleIndex(ix, i);
            bitsA += t32l[p];
            bitsB += t33l[p];
        }
        candidate.big_values         = i;
        candidate.count1table_select = 0;
        if (bitsA > bitsB) {
            bitsA                        = bitsB;
            candidate.count1table_select = 1;
        }
        candidate.count1bits = bitsA;

        if (candidate.block_type == NORM_TYPE) {
            counts.SetEnd(candidate.big_values);
            SplitTry(config, counts, candidate, gi, r01Bits, r01Div, r0Table, r1Table);
        } else {
            candidate.part2_3_length = bitsA;
            std::int32_t a1          = config.scalefac_band.l[7 + 1];
            if (a1 > i)
                a1 = i;
            if (a1 > 0)
                candidate.table_select[0] = ChooseTable(constants, ix, ix + a1, &candidate.part2_3_length);
            if (i > a1)
                candidate.table_select[1] = ChooseTable(constants, ix + a1, ix + i, &candidate.part2_3_length);
            if (gi.part2_3_length > candidate.part2_3_length)
                std::memcpy(&gi, &candidate, sizeof(GrInfo));
        }
    }

    void BestScalefacStore(std::int32_t gr, std::int32_t ch, SideInfo& side) noexcept
    {
        GrInfo& gi     = side.tt[gr][ch];
        bool    recalc = false;

        // bands that quantize to zero need no scalefactor (an idea from AAC); -2 marks them,
        // only this function and ScfsiCalc know the magic number
        std::int32_t j = 0;
        for (std::int32_t sfb = 0; sfb < gi.sfbmax; ++sfb) {
            const std::int32_t width = gi.width[sfb];
            std::int32_t       l     = j;
            j += width;
            while (l < j && gi.l3_enc[l] == 0)
                ++l;
            if (l == j) {
                gi.scalefac[sfb] = -2;
                recalc           = true;
            }
        }

        // all scalefactors even: halve them and use scalefac_scale
        if (!gi.scalefac_scale && !gi.preflag) {
            std::int32_t bits = 0;
            for (std::int32_t sfb = 0; sfb < gi.sfbmax; ++sfb) {
                if (gi.scalefac[sfb] > 0)
                    bits |= gi.scalefac[sfb];
            }
            if (!(bits & 1) && bits != 0) {
                for (std::int32_t sfb = 0; sfb < gi.sfbmax; ++sfb) {
                    if (gi.scalefac[sfb] > 0)
                        gi.scalefac[sfb] >>= 1;
                }
                gi.scalefac_scale = 1;
                recalc            = true;
            }
        }

        // the upper bands all reach the preemphasis: use preflag
        if (!gi.preflag && gi.block_type != SHORT_TYPE) {
            std::int32_t sfb = 11;
            while (sfb < SBPSY_l && !(gi.scalefac[sfb] < pretab[sfb] && gi.scalefac[sfb] != -2))
                ++sfb;
            if (sfb == SBPSY_l) {
                for (sfb = 11; sfb < SBPSY_l; ++sfb) {
                    if (gi.scalefac[sfb] > 0)
                        gi.scalefac[sfb] -= pretab[sfb];
                }
                gi.preflag = 1;
                recalc     = true;
            }
        }

        for (std::int32_t i = 0; i < 4; ++i)
            side.scfsi[ch][i] = 0;

        if (gr == 1 && side.tt[0][ch].block_type != SHORT_TYPE && side.tt[1][ch].block_type != SHORT_TYPE) {
            ScfsiCalc(ch, side);
            recalc = false;
        }
        for (std::int32_t sfb = 0; sfb < gi.sfbmax; ++sfb) {
            if (gi.scalefac[sfb] == -2)
                gi.scalefac[sfb] = 0; // anything goes, so 0 is a good choice
        }
        if (recalc)
            (void)ScaleBitcount(gi);
    }

    bool ScaleBitcount(GrInfo& gi) noexcept
    {
        std::int32_t* const scalefac = gi.scalefac.data();
        const std::int32_t* bitsTable;

        if (gi.block_type == SHORT_TYPE) {
            bitsTable = gi.mixed_block_flag ? scale_mixed.data() : scale_short.data();
        } else {
            bitsTable = scale_long.data();
            if (!gi.preflag) {
                std::int32_t sfb = 11;
                while (sfb < SBPSY_l && scalefac[sfb] >= pretab[sfb])
                    ++sfb;
                if (sfb == SBPSY_l) {
                    gi.preflag = 1;
                    for (sfb = 11; sfb < SBPSY_l; ++sfb)
                        scalefac[sfb] -= pretab[sfb];
                }
            }
        }

        std::int32_t maxSlen1 = 0;
        std::int32_t maxSlen2 = 0;
        std::int32_t sfb      = 0;
        for (; sfb < gi.sfbdivide; ++sfb) {
            if (maxSlen1 < scalefac[sfb])
                maxSlen1 = scalefac[sfb];
        }
        for (; sfb < gi.sfbmax; ++sfb) {
            if (maxSlen2 < scalefac[sfb])
                maxSlen2 = scalefac[sfb];
        }

        // all values of scalefac_compress, the cheapest one wins (ISO would take the first)
        gi.part2_length = LARGE_BITS;
        for (std::int32_t k = 0; k < 16; ++k) {
            if (maxSlen1 < slen1_n[k] && maxSlen2 < slen2_n[k] && gi.part2_length > bitsTable[k]) {
                gi.part2_length      = bitsTable[k];
                gi.scalefac_compress = k;
            }
        }
        return gi.part2_length == LARGE_BITS;
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
