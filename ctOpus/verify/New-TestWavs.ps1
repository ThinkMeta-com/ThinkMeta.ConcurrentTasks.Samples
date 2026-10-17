<#
.SYNOPSIS
    Writes the synthetic test WAV files of the bit-identity check (Verify-Opus.ps1).

.DESCRIPTION
    The signals are computed, not recorded: tones, chords and sweeps from integer oscillators,
    filtered noise, percussive bursts, a pulse train through formant resonators (speech-like),
    silence, clipping and a file shorter than one frame. Only integer arithmetic is used, so every
    computer writes the same bytes; the script checks them against the SHA-256 list below.

    inputs\synth     mono and stereo, 8 to 96 kHz, 8/16/24/32-bit PCM, 32-bit float, extensible
    inputs\surround  3 to 8 channels (channel masks, with LFE) and 10 channels

.EXAMPLE
    .\New-TestWavs.ps1
#>
param([string]$OutDir = (Join-Path $PSScriptRoot 'inputs'))

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;

public static class CtOpusTestSignals
{
    // xorshift32: the same sequence everywhere
    sealed class Rng
    {
        uint s;
        public Rng(uint seed) { s = seed == 0 ? 0x9E3779B9u : seed; }
        public int Next16() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (int)(s >> 16) - 32768; }
    }

    // triangle from a 32-bit phase: -32768..32767
    static int Tri(uint phase)
    {
        int x = (int)(phase >> 15);            // 0..131071
        return x < 65536 ? x - 32768 : 98303 - x;
    }

    static uint Inc(long milliHz, int rate) { return (uint)((milliHz << 32) / (rate * 1000L)); }

    // one channel of "music": a chord of triangles with slow vibrato, a bass line, filtered noise
    // and decaying bursts; variant changes notes, tempo and mix
    public static int[] Music(int rate, int frames, int variant)
    {
        int[] o = new int[frames];
        long[] notes = { 220000, 277183, 329628, 440000, 554365, 659255, 164814, 196000 };
        uint[] ph = new uint[4];
        uint bassPh = 0, vib = 0;
        Rng rng = new Rng((uint)(12345 + variant * 7919));
        int lp = 0, burst = 0;
        int beat = rate / (2 + variant % 3);
        for (int n = 0; n < frames; n++)
        {
            int bar = (n / (beat * 4)) & 7;
            vib += Inc(5000 + variant * 300, rate);
            int v = Tri(vib) >> 9;                       // -64..63: vibrato depth
            long sum = 0;
            for (int k = 0; k < 4; k++)
            {
                long f = notes[(bar + k * 2 + variant) & 7] * (k + 1) / 2;
                ph[k] += Inc(f + f * v / 4096, rate);
                sum += Tri(ph[k]) / (k + 2);
            }
            bassPh += Inc(notes[(bar + variant) & 7] / 4, rate);
            sum += Tri(bassPh) / 2;
            lp += (rng.Next16() - lp) >> 3;              // low-passed noise
            sum += lp / 6;
            if (n % beat == 0) burst = 24000;            // percussive burst, exponential decay
            sum += (long)rng.Next16() * burst >> 16;
            burst -= burst >> 9;
            o[n] = Clamp((int)(sum * 3 / 8));
        }
        return o;
    }

    // speech-like: a pulse train with a wandering pitch through two resonators (Q14 biquads)
    public static int[] Speech(int rate, int frames, int variant)
    {
        int[] o = new int[frames];
        // resonator pairs for vowel-like spectra, coefficients fixed (2r cos w, -r^2 in Q14)
        int[,] co = { { 29900, -15900 }, { 22500, -15700 }, { 31800, -16000 }, { 17000, -15300 } };
        int y1a = 0, y2a = 0, y1b = 0, y2b = 0;
        int period = rate / 120, phase = 0;
        Rng rng = new Rng((uint)(777 + variant));
        for (int n = 0; n < frames; n++)
        {
            int syl = (n / (rate / 5)) & 3;              // a "syllable" every 200 ms
            bool voiced = ((n / (rate / 5)) % 5) != 4;  // every fifth one unvoiced
            int x;
            if (voiced) { x = phase == 0 ? 250 : 0; if (++phase >= period) { phase = 0; period = rate / (100 + ((n / (rate / 10)) % 7) * 10 + variant * 5); } }
            else x = rng.Next16() / 256;
            int a1 = co[syl, 0], a2 = co[syl, 1], b1 = co[(syl + 1) & 3, 0], b2 = co[(syl + 1) & 3, 1];
            int ya = x + (int)(((long)a1 * y1a + (long)a2 * y2a) >> 14);
            y2a = y1a; y1a = Clamp(ya);
            int yb = y1a / 2 + (int)(((long)b1 * y1b + (long)b2 * y2b) >> 14);
            y2b = y1b; y1b = Clamp(yb);
            int env = (n % (rate / 5)) < rate / 6 ? 1 : 0;   // pauses between syllables
            o[n] = env * Clamp(y1b * 3);
        }
        return o;
    }

    static int Clamp(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

    // kind: 1 PCM, 3 float; bits 8/16/24/32; ext: WAVE_FORMAT_EXTENSIBLE with mask
    public static void Write(string path, int rate, int bits, int kind, bool ext, uint mask, int[][] ch, int gainShift, bool clip)
    {
        int channels = ch.Length, frames = ch[0].Length, bytes = bits / 8;
        using (var w = new BinaryWriter(File.Create(path)))
        {
            int fmtLen = ext ? 40 : 16, dataLen = frames * channels * bytes;
            w.Write(0x46464952u); w.Write(4 + 8 + fmtLen + 8 + dataLen); w.Write(0x45564157u);
            w.Write(0x20746D66u); w.Write(fmtLen);
            w.Write((ushort)(ext ? 0xFFFE : kind)); w.Write((ushort)channels); w.Write(rate);
            w.Write(rate * channels * bytes); w.Write((ushort)(channels * bytes)); w.Write((ushort)bits);
            if (ext)
            {
                w.Write((ushort)22); w.Write((ushort)bits); w.Write(mask);
                w.Write((uint)kind); w.Write((ushort)0x0000); w.Write((ushort)0x0010);
                w.Write(new byte[] { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 });
            }
            w.Write(0x61746164u); w.Write(dataLen);
            for (int n = 0; n < frames; n++)
                for (int c = 0; c < channels; c++)
                {
                    int s = ch[c][n];
                    if (clip) s = Clamp(s * 3);
                    s = s >> gainShift;
                    if (kind == 3) w.Write((float)s / 32768f);   // exact: s / 2^15
                    else if (bits == 8) w.Write((byte)((s >> 8) + 128));
                    else if (bits == 16) w.Write((short)s);
                    else if (bits == 24) { int v = s << 8; w.Write((byte)v); w.Write((byte)(v >> 8)); w.Write((byte)(v >> 16)); }
                    else w.Write(s << 16);
                }
        }
    }
}
'@

$synth = New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'synth')
$surround = New-Item -ItemType Directory -Force -Path (Join-Path $OutDir 'surround')
function M($rate, $sec, $v) { , [CtOpusTestSignals]::Music($rate, [int]($rate * $sec), $v) }
function S($rate, $sec, $v) { , [CtOpusTestSignals]::Speech($rate, [int]($rate * $sec), $v) }
function W($dir, $name, $rate, $bits, $kind, $ext, $mask, $channels, $shift = 0, $clip = $false) {
    [CtOpusTestSignals]::Write((Join-Path $dir $name), $rate, $bits, $kind, $ext, [uint32]$mask, [int[][]]$channels, $shift, $clip)
}

# mono and stereo
W $synth 'music-44100-stereo-s16.wav' 44100 16 1 $false 0 @((M 44100 20 0), (M 44100 20 1))
W $synth 'music-48000-stereo-s16.wav' 48000 16 1 $false 0 @((M 48000 10 2), (M 48000 10 3))
W $synth 'music-96000-stereo-s24.wav' 96000 24 1 $false 0 @((M 96000 6 4), (M 96000 6 5))
W $synth 'music-32000-mono-s16.wav' 32000 16 1 $false 0 @(, (M 32000 8 6))
W $synth 'music-22050-stereo-f32.wav' 22050 32 3 $false 0 @((M 22050 8 7), (M 22050 8 0))
W $synth 'music-44100-stereo-u8.wav' 44100 8 1 $false 0 @((M 44100 5 1), (M 44100 5 2))
W $synth 'music-48000-stereo-f32ext.wav' 48000 32 3 $true 3 @((M 48000 6 3), (M 48000 6 4))
W $synth 'speech-16000-mono-s16.wav' 16000 16 1 $false 0 @(, (S 16000 10 0))
W $synth 'speech-8000-mono-s16.wav' 8000 16 1 $false 0 @(, (S 8000 8 1))
W $synth 'speech-24000-stereo-s24ext.wav' 24000 24 1 $true 3 @((S 24000 6 2), (S 24000 6 3))
W $synth 'speech-12000-mono-s16.wav' 12000 16 1 $false 0 @(, (S 12000 6 4))
W $synth 'quiet-44100-stereo-s16.wav' 44100 16 1 $false 0 @((M 44100 5 5), (M 44100 5 6)) 8
W $synth 'clipped-44100-stereo-s16.wav' 44100 16 1 $false 0 @((M 44100 5 7), (M 44100 5 0)) 0 $true
W $synth 'silence-44100-stereo-s16.wav' 44100 16 1 $false 0 @([int[]]::new(44100 * 3), [int[]]::new(44100 * 3))
W $synth 'short-44100-stereo-s16.wav' 44100 16 1 $false 0 @((M 44100 0.01 1), (M 44100 0.01 2))

# multichannel: channel masks of the usual layouts (FL FR FC LFE BL BR FLC FRC BC SL SR)
$layouts = @(@(3, 0x7), @(4, 0x33), @(5, 0x37), @(6, 0x3F), @(7, 0x13F), @(8, 0x63F))
foreach ($l in $layouts) {
    $n = $l[0]; $rate = if ($n % 2) { 44100 } else { 48000 }
    $ch = for ($c = 0; $c -lt $n; $c++) { if ($c -eq 3 -and $n -ge 6) { M $rate 6 0 } else { M $rate 6 ($c + $n) } }
    W $surround ("surround-{0}ch-{1}-s16.wav" -f $n, $rate) $rate 16 1 $true $l[1] $ch
}
W $surround 'surround-6ch-48000-f32.wav' 48000 32 3 $true 0x3F @(for ($c = 0; $c -lt 6; $c++) { M 48000 6 ($c + 3) })
W $surround 'surround-10ch-48000-s16.wav' 48000 16 1 $false 0 @(for ($c = 0; $c -lt 10; $c++) { M 48000 6 $c })

# the bytes every computer must write
$expected = @{}
$listFile = Join-Path $PSScriptRoot 'TestWavs.sha256'
if (Test-Path $listFile) { foreach ($line in Get-Content $listFile) { $p = $line -split '\s+', 2; if ($p.Count -eq 2) { $expected[$p[1]] = $p[0] } } }
$bad = 0
foreach ($f in Get-ChildItem -Recurse -File $OutDir -Filter *.wav | Sort-Object FullName) {
    $rel = $f.FullName.Substring($OutDir.Length + 1).Replace('\', '/')
    $sha = [System.Security.Cryptography.SHA256]::Create(); $s = [IO.File]::OpenRead($f.FullName)
    try { $h = [BitConverter]::ToString($sha.ComputeHash($s)).Replace('-', '').ToLowerInvariant() } finally { $s.Dispose(); $sha.Dispose() }
    $state = if (-not $expected.ContainsKey($rel)) { 'new' } elseif ($expected[$rel] -eq $h) { 'ok' } else { $bad++; 'DIFFERENT' }
    '{0,-44} {1,9} bytes  {2}' -f $rel, $f.Length, $state
}
if ($bad) { throw "$bad test files differ from TestWavs.sha256" }
