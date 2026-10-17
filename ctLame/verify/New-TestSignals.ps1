<#
.SYNOPSIS
    Writes the synthetic test signals of verify\: WAV files computed with integer arithmetic only.

.DESCRIPTION
    Every sample comes from integer phase accumulators, a linear congruential generator and integer
    filters, so the files are byte for byte the same on every machine (checked by the SHA-256 list
    below, and by Reference-Hashes-Signals.csv, whose encodes only match on identical input). No
    recorded audio is used.

    The set covers the input shapes the encoder handles: 32, 44.1 and 48 kHz, mono and stereo,
    8/16/24/32-bit PCM, 32-bit float, WAVE_FORMAT_EXTENSIBLE, files shorter than one frame, silence
    and clipping; and signals that drive the encoder's decisions: tones and sweeps (long blocks,
    sparse spectra), noise (dense spectra, high bit demand), transients (short blocks), different
    left and right channels (mid/side decisions).

.EXAMPLE
    .\New-TestSignals.ps1 -OutDir $env:TEMP\signals
#>
#Requires -Version 7
param([Parameter(Mandatory)][string]$OutDir)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;

public static class CtLameTestSignals
{
    const int Seconds = 8;

    // Numerical Recipes LCG; the upper 16 bits as a signed sample
    sealed class Lcg
    {
        uint state;
        public Lcg(uint seed) { state = seed; }
        public int Next() { state = state * 1664525u + 1013904223u; return (int)(state >> 16) - 32768; }
    }

    // floor division, as Python's //
    static int Div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

    // triangle wave of a 32-bit phase, -32768..32767
    static int Triangle(uint phase)
    {
        int v = (int)(phase >> 15);   // 0..131071
        v = v < 65536 ? v : 131071 - v;
        return v - 32768;
    }

    // 32-bit phase increment of a tone at hz
    static long Step(int rate, int hz) { return ((long)hz << 32) / rate; }

    // left: chord of three triangle tones; right: the same chord a fifth higher
    static void Tones(int rate, int n, out int[] left, out int[] right)
    {
        left = new int[n]; right = new int[n];
        uint[] pl = new uint[3], pr = new uint[3];
        long[] sl = { Step(rate, 220), Step(rate, 277), Step(rate, 330) };
        long[] sr = { Step(rate, 330), Step(rate, 415), Step(rate, 495) };
        for (int i = 0; i < n; i++)
        {
            left[i] = Div(Triangle(pl[0]) + Triangle(pl[1]) + Triangle(pl[2]), 4);
            right[i] = Div(Triangle(pr[0]) + Triangle(pr[1]) + Triangle(pr[2]), 4);
            for (int k = 0; k < 3; k++) { pl[k] = unchecked(pl[k] + (uint)sl[k]); pr[k] = unchecked(pr[k] + (uint)sr[k]); }
        }
    }

    // triangle sweep from 50 Hz to rate/2 on both channels, right inverted
    static void Sweep(int rate, int n, out int[] left, out int[] right)
    {
        left = new int[n]; right = new int[n];
        uint phase = 0;
        long inc = Step(rate, 50);
        long top = Step(rate, rate / 2);
        long grow = (top - inc) / n;
        for (int i = 0; i < n; i++)
        {
            int v = Div(Triangle(phase), 2);
            left[i] = v;
            right[i] = -v;
            phase = unchecked(phase + (uint)inc);
            inc += grow;
        }
    }

    // white noise left, low-passed noise right (one-pole integer filter), independent
    static void Noise(int rate, int n, out int[] left, out int[] right)
    {
        left = new int[n]; right = new int[n];
        Lcg a = new Lcg(1), b = new Lcg(2);
        int y = 0;
        for (int i = 0; i < n; i++)
        {
            left[i] = Div(a.Next(), 2);
            y += (b.Next() - y) >> 3;
            right[i] = y;
        }
    }

    static int Clamp(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

    // noise bursts with a fast attack and an exponential decay every 0.25 s over a quiet tone
    static void Transients(int rate, int n, out int[] left, out int[] right)
    {
        left = new int[n]; right = new int[n];
        Lcg g = new Lcg(3);
        int period = rate / 4;
        uint phase = 0;
        long inc = Step(rate, 440);
        int env = 0;
        for (int i = 0; i < n; i++)
        {
            if (i % period == 0) env = 32767;
            int burst = (g.Next() * env) >> 15;
            env -= env >> 7;
            int tone = Triangle(phase) >> 4;
            phase = unchecked(phase + (uint)inc);
            left[i] = Clamp(burst + tone);
            right[i] = Clamp((burst >> 1) + tone);
        }
    }

    // everything at once: chord, sweep and noise
    static void Mix(int rate, int n, out int[] left, out int[] right)
    {
        int[] tl, tr, sl, sr, zl, zr;
        Tones(rate, n, out tl, out tr); Sweep(rate, n, out sl, out sr); Noise(rate, n, out zl, out zr);
        left = new int[n]; right = new int[n];
        for (int i = 0; i < n; i++)
        {
            left[i] = Div(tl[i] + sl[i] + zr[i], 3);
            right[i] = Div(tr[i] + sr[i] + Div(zl[i], 2), 3);
        }
    }

    delegate void Generator(int rate, int n, out int[] left, out int[] right);

    static short[] Stereo(Generator g, int rate)
    {
        int[] l, r; g(rate, rate * Seconds, out l, out r);
        short[] o = new short[l.Length * 2];
        for (int i = 0; i < l.Length; i++) { o[2 * i] = checked((short)l[i]); o[2 * i + 1] = checked((short)r[i]); }
        return o;
    }

    static short[] Mono(Generator g, int rate)
    {
        int[] l, r; g(rate, rate * Seconds, out l, out r);
        short[] o = new short[l.Length];
        for (int i = 0; i < l.Length; i++) o[i] = checked((short)Div(l[i] + r[i], 2));
        return o;
    }

    static byte[] Pcm16(short[] s, int count)
    {
        byte[] o = new byte[count * 2];
        Buffer.BlockCopy(s, 0, o, 0, o.Length);
        return o;
    }

    static byte[] Pcm8(short[] s)
    {
        byte[] o = new byte[s.Length];
        for (int i = 0; i < s.Length; i++) o[i] = (byte)((s[i] >> 8) + 128);
        return o;
    }

    static byte[] Pcm24(short[] s)
    {
        byte[] o = new byte[s.Length * 3];
        for (int i = 0; i < s.Length; i++)
        {
            int v = s[i] * 256 + ((s[i] * 37) & 0xFF);   // non-zero low byte, so the extra bits matter
            o[3 * i] = (byte)v; o[3 * i + 1] = (byte)(v >> 8); o[3 * i + 2] = (byte)(v >> 16);
        }
        return o;
    }

    static byte[] Pcm32(short[] s)
    {
        byte[] o = new byte[s.Length * 4];
        for (int i = 0; i < s.Length; i++)
            BitConverter.GetBytes(s[i] * 65536 + ((s[i] * 7919) & 0xFFFF)).CopyTo(o, 4 * i);
        return o;
    }

    static byte[] Float32(short[] s)
    {
        byte[] o = new byte[s.Length * 4];
        for (int i = 0; i < s.Length; i++) BitConverter.GetBytes((float)(s[i] / 32768.0)).CopyTo(o, 4 * i);   // exact
        return o;
    }

    static short[] Clipped(short[] s, int gain)
    {
        short[] o = new short[s.Length];
        for (int i = 0; i < s.Length; i++) o[i] = (short)Clamp(s[i] * gain);
        return o;
    }

    static void Chunk(BinaryWriter w, string tag, byte[] payload)
    {
        w.Write(System.Text.Encoding.ASCII.GetBytes(tag)); w.Write((uint)payload.Length); w.Write(payload);
        if (payload.Length % 2 != 0) w.Write((byte)0);
    }

    static void WriteWav(string path, byte[] payload, int rate, int channels, int bits, int formatTag, bool extensible)
    {
        int blockAlign = channels * bits / 8;
        var fmt = new MemoryStream();
        var f = new BinaryWriter(fmt);
        f.Write((ushort)(extensible ? 0xFFFE : formatTag)); f.Write((ushort)channels); f.Write((uint)rate);
        f.Write((uint)(rate * blockAlign)); f.Write((ushort)blockAlign); f.Write((ushort)bits);
        if (extensible)
        {
            // cbSize, valid bits, channel mask, subformat (the format tag, then the fixed GUID tail)
            f.Write((ushort)22); f.Write((ushort)bits); f.Write((uint)(channels == 2 ? 0x3 : 0x4));
            f.Write((uint)formatTag);
            f.Write(new byte[] { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 });
        }
        var body = new MemoryStream();
        var b = new BinaryWriter(body);
        b.Write(System.Text.Encoding.ASCII.GetBytes("WAVE"));
        Chunk(b, "fmt ", fmt.ToArray());
        Chunk(b, "data", payload);
        using (var w = new BinaryWriter(File.Create(path)))
        {
            w.Write(System.Text.Encoding.ASCII.GetBytes("RIFF")); w.Write((uint)body.Length); w.Write(body.ToArray());
        }
    }

    public static string[] WriteAll(string dir)
    {
        short[] mix44 = Stereo(Mix, 44100);
        short[] mix48 = Stereo(Mix, 48000);
        var files = new List<Tuple<string, byte[], int, int, int, int, bool>>
        {
            // 16-bit stereo 44.1 kHz: one file per kind of signal
            Tuple.Create("tones-s16-stereo-44100.wav", Pcm16(Stereo(Tones, 44100), 44100 * Seconds * 2), 44100, 2, 16, 1, false),
            Tuple.Create("sweep-s16-stereo-44100.wav", Pcm16(Stereo(Sweep, 44100), 44100 * Seconds * 2), 44100, 2, 16, 1, false),
            Tuple.Create("noise-s16-stereo-44100.wav", Pcm16(Stereo(Noise, 44100), 44100 * Seconds * 2), 44100, 2, 16, 1, false),
            Tuple.Create("transients-s16-stereo-44100.wav", Pcm16(Stereo(Transients, 44100), 44100 * Seconds * 2), 44100, 2, 16, 1, false),
            Tuple.Create("mix-s16-stereo-44100.wav", Pcm16(mix44, mix44.Length), 44100, 2, 16, 1, false),
            // other rates and channel counts
            Tuple.Create("mix-s16-stereo-32000.wav", Pcm16(Stereo(Mix, 32000), 32000 * Seconds * 2), 32000, 2, 16, 1, false),
            Tuple.Create("mix-s16-stereo-48000.wav", Pcm16(mix48, mix48.Length), 48000, 2, 16, 1, false),
            Tuple.Create("mix-s16-mono-44100.wav", Pcm16(Mono(Mix, 44100), 44100 * Seconds), 44100, 1, 16, 1, false),
            Tuple.Create("transients-s16-mono-32000.wav", Pcm16(Mono(Transients, 32000), 32000 * Seconds), 32000, 1, 16, 1, false),
            Tuple.Create("tones-s16-mono-48000.wav", Pcm16(Mono(Tones, 48000), 48000 * Seconds), 48000, 1, 16, 1, false),
            // other sample formats
            Tuple.Create("mix-u8-stereo-44100.wav", Pcm8(mix44), 44100, 2, 8, 1, false),
            Tuple.Create("mix-s24-stereo-44100.wav", Pcm24(mix44), 44100, 2, 24, 1, false),
            Tuple.Create("mix-s24-stereo-48000-ext.wav", Pcm24(mix48), 48000, 2, 24, 1, true),
            Tuple.Create("mix-s32-stereo-44100.wav", Pcm32(mix44), 44100, 2, 32, 1, false),
            Tuple.Create("mix-f32-stereo-44100.wav", Float32(mix44), 44100, 2, 32, 3, false),
            Tuple.Create("mix-f32-mono-48000-ext.wav", Float32(Mono(Mix, 48000)), 48000, 1, 32, 3, true),
            // edge cases
            Tuple.Create("mix-s16-stereo-44100-short.wav", Pcm16(mix44, 2 * 500), 44100, 2, 16, 1, false),
            Tuple.Create("mix-s16-stereo-44100-onegranule.wav", Pcm16(mix44, 2 * 576), 44100, 2, 16, 1, false),
            Tuple.Create("silence-s16-stereo-44100.wav", new byte[mix44.Length * 2], 44100, 2, 16, 1, false),
            Tuple.Create("mix-s16-stereo-44100-clipped.wav", Pcm16(Clipped(mix44, 6), mix44.Length), 44100, 2, 16, 1, false),
        };
        var names = new List<string>();
        foreach (var t in files)
        {
            WriteWav(Path.Combine(dir, t.Item1), t.Item2, t.Item3, t.Item4, t.Item5, t.Item6, t.Item7);
            names.Add(t.Item1);
        }
        return names.ToArray();
    }
}
'@

# the bytes every computer must write
$expected = @{
    'tones-s16-stereo-44100.wav'           = 'dd18437e70e5fc3ee4703084667e6df80a2f74487f9a4ed9cc1764a83316351c'
    'sweep-s16-stereo-44100.wav'           = '82f4d7ce707d0d219226aae444fa18d58834dbb1c7ea5c322eaaa0eb6c768587'
    'noise-s16-stereo-44100.wav'           = 'ee8fb8a3817125575cbfd42899b2ec360ed30346ef360328c1a6dd116ebf0dc5'
    'transients-s16-stereo-44100.wav'      = '808d3a9248d6c6e3c19e6e1c1fbb7bfce8d8b5a08bdb28b0ce147961ccfdd474'
    'mix-s16-stereo-44100.wav'             = '36730105b769c505ca5c4b3714da992a0f72201a8d7e46bb2baca5777160119b'
    'mix-s16-stereo-32000.wav'             = '3d2349510eb7e3cf134d73a9a92996bd38e645fcbf4e84c9145d528b0d6ab98b'
    'mix-s16-stereo-48000.wav'             = '3ce2a8c0cca8f3faa7c644988bdfddcc950e4cd35f87504978b5e9c62bfa40ce'
    'mix-s16-mono-44100.wav'               = '3a6a8e370da219e421099fe0afcad72ff2dc2201be4d4c4bf2fc3e438ec25994'
    'transients-s16-mono-32000.wav'        = '4742925c1f948d8bc683f4621f3cf7fc45e1de9f4ea4c7f2f0729f42bece45e2'
    'tones-s16-mono-48000.wav'             = '33f48e22491d992da9600626a22ffc99a34e313c063f817c1eeddfed31624543'
    'mix-u8-stereo-44100.wav'              = '12ca3cd6e1104008fe76bd769c365c5fe04a7cba2fb02d8bdb60b5cc33468734'
    'mix-s24-stereo-44100.wav'             = '413833af4e11590cb8a74f40f5f4790b27f3ff6af150252a232af1de090d9136'
    'mix-s24-stereo-48000-ext.wav'         = '5cefbd484de7fc30472094f0dd281e856c23695627c6215ff712e7776b8e7aef'
    'mix-s32-stereo-44100.wav'             = 'c83a2cd39ff3bc7c46491c9fcacf5cced3d5cf8ce65c006c692788b0ea1a34b3'
    'mix-f32-stereo-44100.wav'             = 'a286f979ee63f751acbceaabad1ec08c8e22c1a5a73ad680960d46777356c3d1'
    'mix-f32-mono-48000-ext.wav'           = '77d2f3ac0c5f6df05b238dc4e1f046a7b2531c4f6cebc798d3bcced9efe93c6a'
    'mix-s16-stereo-44100-short.wav'       = 'a3bf3e01bed10d256a4c5b41648a684a68034eb75215e793368c9a139a16105c'
    'mix-s16-stereo-44100-onegranule.wav'  = 'cefa32eb15cb8e0b2eec729e48ad1b225112904a010c2ed600f196b7f0c85fb4'
    'silence-s16-stereo-44100.wav'         = '1918846628d4d25823f9b32c75503e1167c680c3cc0821dd950d41b9c8fdd0c7'
    'mix-s16-stereo-44100-clipped.wav'     = 'd8a4853a662f57e879622a69e208af459edbd6e58a6499e18fcf03d3c55d1d40'
}

$dir = New-Item -ItemType Directory -Force -Path $OutDir
$bad = 0
foreach ($name in [CtLameTestSignals]::WriteAll($dir.FullName)) {
    $path = Join-Path $dir.FullName $name
    $sha = [System.Security.Cryptography.SHA256]::Create(); $s = [IO.File]::OpenRead($path)
    try { $h = [BitConverter]::ToString($sha.ComputeHash($s)).Replace('-', '').ToLowerInvariant() } finally { $s.Dispose(); $sha.Dispose() }
    $state = if (-not $expected.ContainsKey($name)) { 'new' } elseif ($expected[$name] -eq $h) { 'ok' } else { $bad++; 'DIFFERENT' }
    '{0} {1} {2}' -f $h, $name, $state
}
if ($bad) { throw "$bad test signals differ from the expected bytes" }
