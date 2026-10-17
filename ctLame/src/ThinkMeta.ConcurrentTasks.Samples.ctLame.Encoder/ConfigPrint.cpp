// ctLame, a port of the LAME 4.0 MP3 encoder (MPEG-1 Layer III, VBR) to ThinkMeta.ConcurrentTasks.
// Copyright (c) 2026 ThinkMeta Software GmbH for the port; LAME's copyrights for the original:
// Copyright (c) 1999-2011 The LAME Project and the authors named in the LAME sources.
// Licensed under the GNU Library General Public License, version 2 or (at your option) any later
// version; see COPYING and ctLame-LICENSE.md.

#include "ConfigPrint.h"

#include <cmath>

#include "SessionConfig.h"
#include "Version.h"
#include "ctLameVersion.h"

namespace ThinkMeta::ConcurrentTasks::Samples::ctLame {

    void PrintConfig(const SessionConfig& cfg)
    {
        const ReportFunction msgf           = cfg.options.report.message;
        const double         out_samplerate = cfg.samplerate_out;

        Report(msgf, "ctLame %s %s, based on LAME %s (%s)\n", CTLAME_VERSION_STRING, GetLameOsBitness(), GetLameVersion(), GetLameUrl());

        if (cfg.channels_in == 2 && cfg.channels_out == 1)
            Report(msgf, "Autoconverting from stereo to mono. Setting encoding to mono mode.\n");

        if (cfg.highpass2 > 0.)
            Report(msgf, "Using polyphase highpass filter, transition band: %5.0f Hz - %5.0f Hz\n", 0.5 * cfg.highpass1 * out_samplerate,
                   0.5 * cfg.highpass2 * out_samplerate);
        if (0. < cfg.lowpass1 || 0. < cfg.lowpass2)
            Report(msgf, "Using polyphase lowpass filter, transition band: %5.0f Hz - %5.0f Hz\n", 0.5 * cfg.lowpass1 * out_samplerate,
                   0.5 * cfg.lowpass2 * out_samplerate);
        else
            Report(msgf, "polyphase lowpass filter disabled\n");
    }

    void PrintInternals(const SessionConfig& cfg)
    {
        const ReportFunction  msgf = cfg.options.report.message;
        const EncoderOptions& gfp  = cfg.options;
        const char*           pc   = "";

        // compiler/processor optimizations, operational, etc.
        Report(msgf, "\nmisc:\n\n");

        Report(msgf, "\tscaling: %g\n", static_cast<double>(gfp.scale));
        Report(msgf, "\tch0 (left) scaling: %g\n", static_cast<double>(gfp.scaleLeft));
        Report(msgf, "\tch1 (right) scaling: %g\n", static_cast<double>(gfp.scaleRight));
        switch (cfg.use_best_huffman) {
            default: pc = "normal"; break;
            case 1: pc = "best (outside loop)"; break;
            case 2: pc = "best (inside loop, slow)"; break;
        }
        Report(msgf, "\thuffman search: %s\n", pc);
        Report(msgf, "\texperimental Y=%d\n", gfp.experimentalY);
        Report(msgf, "\t...\n");

        // everything controlling the stream format
        Report(msgf, "\nstream format:\n\n");
        Report(msgf, "\tMPEG-1 Layer 3\n");
        switch (cfg.mode) {
            case JointStereo: pc = "joint stereo"; break;
            case Stereo: pc = "stereo"; break;
            case DualChannel: pc = "dual channel"; break;
            case Mono: pc = "mono"; break;
            case NotSet: pc = "not set (error)"; break;
            default: pc = "unknown (error)"; break;
        }
        Report(msgf, "\t%d channel - %s\n", cfg.channels_out, pc);

        Report(msgf, "\tvariable bitrate - VBR mtrh\n");
        if (cfg.write_lame_tag)
            Report(msgf, "\tusing LAME Tag\n");
        Report(msgf, "\t...\n");

        // everything controlling psychoacoustic settings, like ATH, etc.
        Report(msgf, "\npsychoacoustic:\n\n");

        switch (cfg.short_blocks) {
            default:
            case ShortBlockNotSet: pc = "?"; break;
            case ShortBlockAllowed: pc = "allowed"; break;
            case ShortBlockCoupled: pc = "channel coupled"; break;
            case ShortBlockDispensed: pc = "dispensed"; break;
            case ShortBlockForced: pc = "forced"; break;
        }
        Report(msgf, "\tusing short blocks: %s\n", pc);
        Report(msgf, "\tsubblock gain: %d\n", cfg.subblock_gain);
        Report(msgf, "\tadjust masking: %g dB\n", static_cast<double>(cfg.mask_adjust));
        Report(msgf, "\tadjust masking short: %g dB\n", static_cast<double>(cfg.mask_adjust_short));
        Report(msgf, "\tnoise shaping: %d\n", cfg.noise_shaping);

        pc = "using";
        if (cfg.ATHshort)
            pc = "the only masking for short blocks";
        if (cfg.ATHonly)
            pc = "the only masking";
        if (cfg.noATH)
            pc = "not used";
        Report(msgf, "\tATH: %s\n", pc);
        Report(msgf, "\t ^ type: %d\n", cfg.ATHtype);
        Report(msgf, "\t ^ shape: %g%s\n", static_cast<double>(cfg.ATHcurve), " (only for type 4)");
        Report(msgf, "\t ^ level adjustement: %g dB\n", static_cast<double>(cfg.ATH_offset_db));
        Report(msgf, "\t ^ adjust type: %d\n", cfg.ath_use_adjust);
        Report(msgf, "\t ^ adjust sensitivity power: %f\n", static_cast<double>(cfg.ath_aa_sensitivity_p));

        Report(msgf, "\texperimental psy tunings by Naoki Shibata\n");
        // log10 of a float in C promotes to double; std::log10(float) would stay in float
        Report(msgf, "\t   adjust masking bass=%g dB, alto=%g dB, treble=%g dB, sfb21=%g dB\n", 10 * std::log10(static_cast<double>(cfg.longfact[0])),
               10 * std::log10(static_cast<double>(cfg.longfact[7])), 10 * std::log10(static_cast<double>(cfg.longfact[14])),
               10 * std::log10(static_cast<double>(cfg.longfact[21])));

        pc = cfg.use_temporal_masking_effect ? "yes" : "no";
        Report(msgf, "\tusing temporal masking effect: %s\n", pc);
        Report(msgf, "\tinterchannel masking ratio: %g\n", static_cast<double>(cfg.interChRatio));
        Report(msgf, "\t...\n");

        Report(msgf, "\n");
    }

} // namespace ThinkMeta::ConcurrentTasks::Samples::ctLame
