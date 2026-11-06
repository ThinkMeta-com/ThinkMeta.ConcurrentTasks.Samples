// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "ConfigPrint.h"

#include <cmath>

#include "Console.h"
#include "SessionConfig.h"
#include "Version.h"
#include "ctLameVersion.h"

namespace ThinkMeta::ConcurrentTasks::Samples::Lame {

    using Shared::Console;
    using Shared::Stream;

    void PrintConfig(const SessionConfig& cfg)
    {
        const double out_samplerate = cfg.samplerate_out;

        Console::Print(Stream::Error, CT_TEXT("ctLame %hs %hs, based on LAME %hs (%hs)\n"), CTLAME_VERSION_STRING, GetLameOsBitness(), GetLameVersion(),
                       GetLameUrl());

        if (cfg.channels_in == 2 && cfg.channels_out == 1)
            Console::Print(Stream::Error, CT_TEXT("Autoconverting from stereo to mono. Setting encoding to mono mode.\n"));

        if (cfg.highpass2 > 0.)
            Console::Print(Stream::Error, CT_TEXT("Using polyphase highpass filter, transition band: %5.0f Hz - %5.0f Hz\n"),
                           0.5 * cfg.highpass1 * out_samplerate, 0.5 * cfg.highpass2 * out_samplerate);
        if (0. < cfg.lowpass1 || 0. < cfg.lowpass2)
            Console::Print(Stream::Error, CT_TEXT("Using polyphase lowpass filter, transition band: %5.0f Hz - %5.0f Hz\n"),
                           0.5 * cfg.lowpass1 * out_samplerate, 0.5 * cfg.lowpass2 * out_samplerate);
        else
            Console::Print(Stream::Error, CT_TEXT("polyphase lowpass filter disabled\n"));
    }

    void PrintInternals(const SessionConfig& cfg)
    {
        const EncoderOptions& gfp = cfg.options;
        const char*           pc  = "";

        // compiler/processor optimizations, operational, etc.
        Console::Print(Stream::Error, CT_TEXT("\nmisc:\n\n"));

        Console::Print(Stream::Error, CT_TEXT("\tscaling: %g\n"), static_cast<double>(gfp.scale));
        Console::Print(Stream::Error, CT_TEXT("\tch0 (left) scaling: %g\n"), static_cast<double>(gfp.scaleLeft));
        Console::Print(Stream::Error, CT_TEXT("\tch1 (right) scaling: %g\n"), static_cast<double>(gfp.scaleRight));
        switch (cfg.use_best_huffman) {
            default: pc = "normal"; break;
            case 1: pc = "best (outside loop)"; break;
            case 2: pc = "best (inside loop, slow)"; break;
        }
        Console::Print(Stream::Error, CT_TEXT("\thuffman search: %hs\n"), pc);
        Console::Print(Stream::Error, CT_TEXT("\texperimental Y=%d\n"), gfp.experimentalY);
        Console::Print(Stream::Error, CT_TEXT("\t...\n"));

        // everything controlling the stream format
        Console::Print(Stream::Error, CT_TEXT("\nstream format:\n\n"));
        Console::Print(Stream::Error, CT_TEXT("\tMPEG-1 Layer 3\n"));
        switch (cfg.mode) {
            case JointStereo: pc = "joint stereo"; break;
            case Stereo: pc = "stereo"; break;
            case DualChannel: pc = "dual channel"; break;
            case Mono: pc = "mono"; break;
            case NotSet: pc = "not set (error)"; break;
            default: pc = "unknown (error)"; break;
        }
        Console::Print(Stream::Error, CT_TEXT("\t%d channel - %hs\n"), cfg.channels_out, pc);

        Console::Print(Stream::Error, CT_TEXT("\tvariable bitrate - VBR mtrh\n"));
        if (cfg.write_lame_tag)
            Console::Print(Stream::Error, CT_TEXT("\tusing LAME Tag\n"));
        Console::Print(Stream::Error, CT_TEXT("\t...\n"));

        // everything controlling psychoacoustic settings, like ATH, etc.
        Console::Print(Stream::Error, CT_TEXT("\npsychoacoustic:\n\n"));

        switch (cfg.short_blocks) {
            default:
            case ShortBlockNotSet: pc = "?"; break;
            case ShortBlockAllowed: pc = "allowed"; break;
            case ShortBlockCoupled: pc = "channel coupled"; break;
            case ShortBlockDispensed: pc = "dispensed"; break;
            case ShortBlockForced: pc = "forced"; break;
        }
        Console::Print(Stream::Error, CT_TEXT("\tusing short blocks: %hs\n"), pc);
        Console::Print(Stream::Error, CT_TEXT("\tsubblock gain: %d\n"), cfg.subblock_gain);
        Console::Print(Stream::Error, CT_TEXT("\tadjust masking: %g dB\n"), static_cast<double>(cfg.mask_adjust));
        Console::Print(Stream::Error, CT_TEXT("\tadjust masking short: %g dB\n"), static_cast<double>(cfg.mask_adjust_short));
        Console::Print(Stream::Error, CT_TEXT("\tnoise shaping: %d\n"), cfg.noise_shaping);

        pc = "using";
        if (cfg.ATHshort)
            pc = "the only masking for short blocks";
        if (cfg.ATHonly)
            pc = "the only masking";
        if (cfg.noATH)
            pc = "not used";
        Console::Print(Stream::Error, CT_TEXT("\tATH: %hs\n"), pc);
        Console::Print(Stream::Error, CT_TEXT("\t ^ type: %d\n"), cfg.ATHtype);
        Console::Print(Stream::Error, CT_TEXT("\t ^ shape: %g%hs\n"), static_cast<double>(cfg.ATHcurve), " (only for type 4)");
        Console::Print(Stream::Error, CT_TEXT("\t ^ level adjustement: %g dB\n"), static_cast<double>(cfg.ATH_offset_db));
        Console::Print(Stream::Error, CT_TEXT("\t ^ adjust type: %d\n"), cfg.ath_use_adjust);
        Console::Print(Stream::Error, CT_TEXT("\t ^ adjust sensitivity power: %f\n"), static_cast<double>(cfg.ath_aa_sensitivity_p));

        Console::Print(Stream::Error, CT_TEXT("\texperimental psy tunings by Naoki Shibata\n"));
        // log10 of a float in C promotes to double; std::log10(float) would stay in float
        Console::Print(Stream::Error, CT_TEXT("\t   adjust masking bass=%g dB, alto=%g dB, treble=%g dB, sfb21=%g dB\n"),
                       10 * std::log10(static_cast<double>(cfg.longfact[0])), 10 * std::log10(static_cast<double>(cfg.longfact[7])),
                       10 * std::log10(static_cast<double>(cfg.longfact[14])), 10 * std::log10(static_cast<double>(cfg.longfact[21])));

        pc = cfg.use_temporal_masking_effect ? "yes" : "no";
        Console::Print(Stream::Error, CT_TEXT("\tusing temporal masking effect: %hs\n"), pc);
        Console::Print(Stream::Error, CT_TEXT("\tinterchannel masking ratio: %g\n"), static_cast<double>(cfg.interChRatio));
        Console::Print(Stream::Error, CT_TEXT("\t...\n"));

        Console::Print(Stream::Error, CT_TEXT("\n"));
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::Lame
