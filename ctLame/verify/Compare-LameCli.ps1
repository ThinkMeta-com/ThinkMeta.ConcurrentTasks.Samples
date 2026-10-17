<#
.SYNOPSIS
    Compares the command line behaviour of ctLame.exe with the reduced lame.exe.

.DESCRIPTION
    For every case both programs run with the same arguments in their own copy of a work
    directory. Compared are the exit code, stdout, stderr and every MP3 file the case leaves in
    the work directory. argv[0] is "lame" for both, so usage texts match. (The progress display
    and the VBR histogram, whose times differ from run to run, were removed from both programs.)

    The cases cover every option of the reduced frontend (alone and with --verbose, which prints
    the session configuration), the parser quirks (bundled short options, missing arguments,
    LAMEOPT) and WAV header edge cases, generated as small files in the work directory.

    Needs PowerShell 7. Exit code 0 when every case is identical, 1 otherwise.

.EXAMPLE
    .\Compare-LameCli.ps1
    .\Compare-LameCli.ps1 -Lame path\to\lame.exe -CtLame path\to\ctLame.exe -Keep
#>
#Requires -Version 7
param(
    [string]$Lame = (Join-Path $PSScriptRoot '..\build\Release\bin\lame.exe'),
    [string]$CtLame = (Join-Path $PSScriptRoot '..\build\Release\bin\ctLame.exe'),
    [switch]$Keep                                # keep the work directory
)

$ErrorActionPreference = 'Stop'
foreach ($exe in $Lame, $CtLame) { if (-not (Test-Path -LiteralPath $exe)) { throw "$exe not found" } }

Add-Type -TypeDefinition @'
using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

public static class CtLameCliCompare
{
    // --- WAV files ------------------------------------------------------------------------------

    static byte[] Chunk(string tag, byte[] payload)
    {
        var m = new MemoryStream();
        var w = new BinaryWriter(m);
        w.Write(Encoding.ASCII.GetBytes(tag)); w.Write((uint)payload.Length); w.Write(payload);
        if (payload.Length % 2 != 0) w.Write((byte)0);
        return m.ToArray();
    }

    static byte[] FmtPayload(int tag, int channels, int rate, int bits, int? extensibleSub)
    {
        int blockAlign = channels * ((bits + 7) / 8);
        var m = new MemoryStream();
        var w = new BinaryWriter(m);
        w.Write((ushort)tag); w.Write((ushort)channels); w.Write((uint)rate); w.Write((uint)(rate * blockAlign));
        w.Write((ushort)blockAlign); w.Write((ushort)bits);
        if (extensibleSub.HasValue)
        {
            w.Write((ushort)22); w.Write((ushort)bits); w.Write((uint)(channels == 2 ? 3 : 4)); w.Write((ushort)extensibleSub.Value);
            w.Write(new byte[] { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 });
        }
        return m.ToArray();
    }

    static byte[] Wav(int channels = 2, int rate = 44100, int bits = 16, int tag = 1, int frames = 3000, int? extensibleSub = null,
                      uint? dataSize = null, byte[] extraChunks = null)
    {
        int bytesPerSample = (bits + 7) / 8;
        byte[] data = new byte[frames * channels * bytesPerSample];
        for (int i = 0; i < data.Length; i++) data[i] = (byte)((i * 37) & 0xFF);
        var body = new MemoryStream();
        var w = new BinaryWriter(body);
        w.Write(Encoding.ASCII.GetBytes("WAVE"));
        if (extraChunks != null) w.Write(extraChunks);
        w.Write(Chunk("fmt ", FmtPayload(tag, channels, rate, bits, extensibleSub)));
        w.Write(Encoding.ASCII.GetBytes("data")); w.Write(dataSize ?? (uint)data.Length); w.Write(data);
        var file = new MemoryStream();
        var f = new BinaryWriter(file);
        f.Write(Encoding.ASCII.GetBytes("RIFF")); f.Write((uint)body.Length); f.Write(body.ToArray());
        return file.ToArray();
    }

    static byte[] Concat(params byte[][] parts) { return parts.SelectMany(p => p).ToArray(); }
    static byte[] Ascii(string s) { return Encoding.Latin1.GetBytes(s); }

    static void MakeInputs(string directory)
    {
        var files = new List<KeyValuePair<string, byte[]>>
        {
            new KeyValuePair<string, byte[]>("in.wav", Wav()),
            new KeyValuePair<string, byte[]>("IN2.WAV", Wav()),
            new KeyValuePair<string, byte[]>("mono.wav", Wav(channels: 1)),
            new KeyValuePair<string, byte[]>("r32.wav", Wav(rate: 32000)),
            new KeyValuePair<string, byte[]>("r48.wav", Wav(rate: 48000)),
            new KeyValuePair<string, byte[]>("r22.wav", Wav(rate: 22050)),
            new KeyValuePair<string, byte[]>("ch3.wav", Wav(channels: 3)),
            new KeyValuePair<string, byte[]>("ch0.wav", Wav(channels: 0)),
            new KeyValuePair<string, byte[]>("u8.wav", Wav(bits: 8)),
            new KeyValuePair<string, byte[]>("s24.wav", Wav(bits: 24)),
            new KeyValuePair<string, byte[]>("s32.wav", Wav(bits: 32)),
            new KeyValuePair<string, byte[]>("s12.wav", Wav(bits: 12)),
            new KeyValuePair<string, byte[]>("b0.wav", Wav(bits: 0)),
            new KeyValuePair<string, byte[]>("f32.wav", Wav(bits: 32, tag: 3)),
            new KeyValuePair<string, byte[]>("ext16.wav", Wav(bits: 16, tag: 0xFFFE, extensibleSub: 1)),
            new KeyValuePair<string, byte[]>("extf.wav", Wav(bits: 32, tag: 0xFFFE, extensibleSub: 3)),
            new KeyValuePair<string, byte[]>("adpcm.wav", Wav(tag: 2)),
            new KeyValuePair<string, byte[]>("nolen.wav", Wav(dataSize: 0xFFFFFFFF)),
            new KeyValuePair<string, byte[]>("list.wav", Wav(extraChunks: Chunk("LIST", Ascii("abc")))),
            new KeyValuePair<string, byte[]>("empty.wav", Wav(frames: 0)),
            new KeyValuePair<string, byte[]>("notwave.wav", Concat(Ascii("RIFF"), new byte[] { 0x10, 0, 0, 0 }, Ascii("AVI junkjunkjunk"))),
            new KeyValuePair<string, byte[]>("text.wav", Ascii("hello world, not a wave file\n")),
            new KeyValuePair<string, byte[]>("nodata.wav", Concat(Ascii("RIFF"), new byte[] { 0x1c, 0, 0, 0 }, Ascii("WAVE"), Chunk("fmt ", FmtPayload(1, 2, 44100, 16, null)))),
            new KeyValuePair<string, byte[]>("shortfmt.wav", Concat(Ascii("RIFF"), new byte[] { 0x14, 0, 0, 0 }, Ascii("WAVEfmt "), new byte[] { 8, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xac, 0, 0 })),
            new KeyValuePair<string, byte[]>("a.b.c.wav", Wav()),
            new KeyValuePair<string, byte[]>("noext", Wav()),
            new KeyValuePair<string, byte[]>("song.mp3.wav", Wav()),
            new KeyValuePair<string, byte[]>("x.flac", Wav()),
            new KeyValuePair<string, byte[]>("x.xyz", Wav()),
        };
        foreach (var f in files) File.WriteAllBytes(Path.Combine(directory, f.Key), f.Value);
        Directory.CreateDirectory(Path.Combine(directory, "sub"));
        File.WriteAllBytes(Path.Combine(directory, "sub", "in.wav"), Wav());
    }

    // --- cases ----------------------------------------------------------------------------------

    static string[] A(params string[] a) { return a; }

    static readonly string[][] OptionSets =
    {
        A(), A("-V", "0"), A("-V", "2"), A("-V", "4.51"), A("-V", "6.5"), A("-V", "7.3"), A("-V", "9"), A("-V", "9.999"),
        A("-V", "12"), A("-V", "-1"), A("-V0"), A("-V", "abc"),
        A("-q", "0"), A("-q", "1"), A("-q", "2"), A("-q", "5"), A("-q", "7"), A("-q", "8"), A("-q", "9"), A("-q", "-3"), A("-q", "15"),
        A("-h"), A("-f"), A("-hf"), A("-fh"),
        A("--preset", "medium"), A("--preset", "standard"), A("--preset", "extreme"), A("--preset", "bogus"),
        A("--r3mix"), A("--R3MIX"), A("--r3mix", "-V", "5"), A("-V", "5", "--r3mix"),
        A("-b", "128"), A("-b", "100"), A("-b", "400"), A("-b", "8"), A("-B", "256"), A("-B", "999"), A("-b", "128", "-B", "96"),
        A("-b", "320", "--nores"), A("-F"), A("-b", "64", "-F"),
        A("-Y"), A("-Z"), A("-Z", "2"), A("-Z3"), A("--athaa-sensitivity", "3"), A("--athaa-sensitivity", "x"),
        A("--scale", "0.5"), A("--scale", "0"), A("--scale-l", "2"), A("--scale-r", "0"), A("--gain", "6"), A("--gain", "-30"), A("--gain", "20"),
        A("--swap-channel"), A("--ignorelength"), A("-a"),
        A("--lowpass", "16"), A("--lowpass", "16000"), A("--lowpass", "-1"), A("--lowpass", "0"), A("--lowpass", "30"),
        A("--lowpass", "5", "--lowpass-width", "2"), A("--lowpass-width", "0"), A("--highpass", "0.1"), A("--highpass", "0.02"), A("--highpass", "20"),
        A("--highpass", "1", "--highpass-width", "0.5"), A("--highpass", "1", "--highpass-width", "300"), A("--highpass", "-1"),
        A("--highpass", "60000"),
        A("-m", "j"), A("-m", "s"), A("-m", "f"), A("-m", "d"), A("-m", "m"), A("-m", "l"), A("-m", "r"), A("-m", "a"), A("-m", "x"), A("-ms"),
        A("-mf", "-V", "3"),
        A("--flush"), A("--priority", "2"), A("--priority"),
        A("-e", "n"), A("-e", "5"), A("-e", "c"), A("-e", "x"), A("-c"), A("-o"), A("-p"), A("--nores"), A("--strictly-enforce-ISO"),
        A("--buffer-constraint", "default"), A("--buffer-constraint", "strict"), A("--buffer-constraint", "maximum"),
        A("--buffer-constraint", "bogus"), A("-t"), A("-T"), A("-t", "-T"),
        A("--disptime", "1"), A("-S"), A("--nohist"), A("--quiet"), A("--silent"), A("--brief"),
        A("-V2q0"), A("-V", "2", "-q", "0", "-m", "s", "-b", "96", "-B", "224", "-F", "-p", "-c", "-o", "-e", "5"),
        A("--foo"), A("-k"), A("-x"),
    };

    static readonly string[] InputsForAllOptions = { "in.wav", "mono.wav", "r32.wav", "r48.wav" };

    static readonly string[][] ExtraCases =
    {
        A(),
        A("--help"), A("--usage"), A("--longhelp"), A("-?"), A("--?"), A("--version"), A("--license"), A("--preset", "help"),
        A("-V", "2"), A("in.wav", "out.mp3", "extra.mp3"), A("-"), A("in.wav", "-"), A("in.wav", "in.wav"),
        A("in.wav", "-V"), A("in.wav", "-q"), A("in.wav", "-m"), A("in.wav", "-e"), A("in.wav", "-b"), A("in.wav", "--scale"),
        A("in.wav", "--preset"), A("in.wav", "--buffer-constraint"), A("in.wav", "-Z"),
        A("missing.wav"), A("missing.wav", "--quiet"), A("sub\\in.wav"), A("sub\\in.wav", "sub\\out.mp3"),
        A("IN2.WAV"), A("a.b.c.wav"), A("noext"), A("song.mp3.wav"), A("x.flac"), A("x.xyz"), A("in.wav", "nodir\\out.mp3"),
        A("-Zin.wav"), A("-Z", "in.wav"), A("in.wav", "-S", "--verbose"), A("in.wav", "--verbose", "-S"), A("in.wav", "--silent", "--verbose"),
    };

    static readonly string[] HeaderCaseFiles = { "r22.wav", "ch3.wav", "ch0.wav", "u8.wav", "s24.wav", "s32.wav", "s12.wav", "b0.wav", "f32.wav",
        "ext16.wav", "extf.wav", "adpcm.wav", "nolen.wav", "list.wav", "empty.wav", "notwave.wav", "text.wav", "nodata.wav", "shortfmt.wav" };

    // Differences that are expected, with the reason (none left).
    static readonly Dictionary<string, string> KnownDifferences = new Dictionary<string, string>();

    // ctLame's own texts (name, copyright, its options) and the LAME texts they replace. ctLame's
    // stdout and stderr are translated back before the comparison, so everything else is still
    // compared byte for byte. Longer texts first.
    static readonly string[][] CtLameTexts =
    {
        A("ctLame 0.1 64bits, based on LAME 4.0 (https://lame.sourceforge.io)\nCopyright (c) 2026 ThinkMeta Software GmbH\n\n",
          "LAME 64bits version 4.0 (https://lame.sourceforge.io)\n\n"),
        A("ctLame 0.1 64bits, based on LAME 4.0 (https://lame.sourceforge.io)\n", "LAME 4.0 64bits (https://lame.sourceforge.io)\n"),
        A("ctLame is a parallel port of LAME 4.0 (VBR) to ThinkMeta ConcurrentTasks;\n" +
          "its output is bit-identical to LAME's. LAME:\n\n", ""),
        A("    --threads n     encode with n threads (default: all logical processors)\n\n", ""),
        A("  ctLame options (parallel encoding, SIMD):\n" +
          "    --threads <n>   compute threads, 1 to 256 (default: all logical processors)\n" +
          "    --sequential    the whole encoder in one task, without parallel stages\n" +
          "                    (the reference path, slower)\n" +
          "    --simd <level>  the highest SIMD level to use: scalar, sse4.1 or avx2\n" +
          "                    (default: the highest the CPU supports; all levels give\n" +
          "                    the same bits)\n" +
          "    --features      print the CPU features and the SIMD level in use\n", ""),
        A("ctLame encodes MPEG-1", "LAME encodes MPEG-1"),
        A("    ctlame -V2 input.wav output.mp3", "    lame -V2 input.wav output.mp3"),
        A("lets ctLame ignore noise", "lets LAME ignore noise"),
        A("aliases over ctLame settings", "aliases over LAME settings"),
    };

    static string AsLameText(string text)
    {
        foreach (var t in CtLameTexts) text = text.Replace(t[0], t[1]);
        return text;
    }

    sealed class Case { public string[] Args; public string EnvOpt; }

    static List<Case> BuildCases()
    {
        var cases = new List<Case>();
        foreach (var options in OptionSets)
            foreach (var name in InputsForAllOptions)
            {
                cases.Add(new Case { Args = options.Concat(new[] { name, "out.mp3" }).ToArray() });
                cases.Add(new Case { Args = options.Concat(new[] { "--verbose", name, "out.mp3" }).ToArray() });
            }
        foreach (var args in ExtraCases) cases.Add(new Case { Args = args });
        foreach (var name in HeaderCaseFiles)
        {
            cases.Add(new Case { Args = A(name, "out.mp3", "--verbose") });
            cases.Add(new Case { Args = A("-a", name, "out.mp3", "--verbose") });
            cases.Add(new Case { Args = A("--quiet", name, "out.mp3") });
        }
        // LAMEOPT: environment arguments go before the command line arguments
        cases.Add(new Case { Args = A("--verbose", "in.wav", "out.mp3"), EnvOpt = "-V 7 -m m" });
        cases.Add(new Case { Args = A("-V", "1", "in.wav", "out.mp3", "--verbose"), EnvOpt = "-V 7" });
        cases.Add(new Case { Args = A("in.wav", "out.mp3"), EnvOpt = "\"--verbose\"   -q 1" });
        cases.Add(new Case { Args = A("out.mp3"), EnvOpt = "in.wav" });
        return cases;
    }

    // --- running --------------------------------------------------------------------------------

    // The command line of a process, quoted the way the C runtime splits it again (the rules of
    // Python's subprocess.list2cmdline).
    static string CommandLine(IEnumerable<string> args)
    {
        var r = new StringBuilder();
        foreach (var arg in args)
        {
            if (r.Length > 0) r.Append(' ');
            bool quote = arg.Length == 0 || arg.IndexOf(' ') >= 0 || arg.IndexOf('\t') >= 0;
            if (quote) r.Append('"');
            int backslashes = 0;
            foreach (char c in arg)
            {
                if (c == '\\') { backslashes++; continue; }
                if (c == '"') { r.Append('\\', backslashes * 2); backslashes = 0; r.Append("\\\""); continue; }
                r.Append('\\', backslashes); backslashes = 0; r.Append(c);
            }
            r.Append('\\', backslashes);
            if (quote) { r.Append('\\', backslashes); r.Append('"'); }
        }
        return r.ToString();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfo
    {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInformation { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string applicationName, StringBuilder commandLine, IntPtr processAttributes, IntPtr threadAttributes,
        bool inheritHandles, uint creationFlags, IntPtr environment, string currentDirectory, ref StartupInfo startupInfo, out ProcessInformation processInformation);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr handle);

    static readonly object StartLock = new object();

    // Runs exe with argv[0] "lame" in cwd, LAMEOPT removed or set, 16 newlines on stdin.
    static void Run(string exe, string[] args, string envOpt, string cwd, out uint exitCode, out string stdout, out string stderr)
    {
        var env = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (DictionaryEntry e in Environment.GetEnvironmentVariables()) env[(string)e.Key] = (string)e.Value;
        env.Remove("LAMEOPT");
        if (envOpt != null) env["LAMEOPT"] = envOpt;
        var block = new StringBuilder();
        foreach (var e in env) block.Append(e.Key).Append('=').Append(e.Value).Append('\0');
        block.Append('\0');

        var stdinPipe = new AnonymousPipeServerStream(PipeDirection.Out, HandleInheritability.Inheritable);
        var stdoutPipe = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.Inheritable);
        var stderrPipe = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.Inheritable);
        IntPtr envBlock = Marshal.StringToHGlobalUni(block.ToString());
        ProcessInformation pi;
        try
        {
            var si = new StartupInfo();
            si.cb = Marshal.SizeOf(typeof(StartupInfo));
            si.dwFlags = 0x100;   // STARTF_USESTDHANDLES
            si.hStdInput = stdinPipe.ClientSafePipeHandle.DangerousGetHandle();
            si.hStdOutput = stdoutPipe.ClientSafePipeHandle.DangerousGetHandle();
            si.hStdError = stderrPipe.ClientSafePipeHandle.DangerousGetHandle();
            var commandLine = new StringBuilder(CommandLine(new[] { "lame" }.Concat(args)));
            lock (StartLock)
            {
                // CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW
                if (!CreateProcessW(exe, commandLine, IntPtr.Zero, IntPtr.Zero, true, 0x400 | 0x08000000, envBlock, cwd, ref si, out pi))
                    throw new Win32Exception();
            }
        }
        finally
        {
            Marshal.FreeHGlobal(envBlock);
            stdinPipe.DisposeLocalCopyOfClientHandle();
            stdoutPipe.DisposeLocalCopyOfClientHandle();
            stderrPipe.DisposeLocalCopyOfClientHandle();
        }
        try
        {
            var outBuffer = new MemoryStream();
            var errBuffer = new MemoryStream();
            Task readOut = stdoutPipe.CopyToAsync(outBuffer), readErr = stderrPipe.CopyToAsync(errBuffer);
            try { stdinPipe.Write(Encoding.ASCII.GetBytes(new string('\n', 16))); } catch (IOException) { }
            stdinPipe.Dispose();
            if (WaitForSingleObject(pi.hProcess, 120000) != 0)
            {
                TerminateProcess(pi.hProcess, 1);
                throw new TimeoutException("lame " + string.Join(" ", args) + " timed out after 120 seconds");
            }
            Task.WaitAll(readOut, readErr);
            GetExitCodeProcess(pi.hProcess, out exitCode);
            // stdout and stderr are text streams: the CRT writes "\r\n", a lone "\r" is the progress display
            stdout = Encoding.Latin1.GetString(outBuffer.ToArray()).Replace("\r\n", "\n");
            stderr = Encoding.Latin1.GetString(errBuffer.ToArray()).Replace("\r\n", "\n");
        }
        finally
        {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            stdoutPipe.Dispose(); stderrPipe.Dispose();
        }
    }

    // The MP3 files a case produced, by relative path; removes them for the next case.
    static SortedDictionary<string, byte[]> TakeMp3Files(string directory)
    {
        var files = new SortedDictionary<string, byte[]>(StringComparer.Ordinal);
        foreach (var path in Directory.GetFiles(directory, "*", SearchOption.AllDirectories))
        {
            if (!path.EndsWith(".mp3", StringComparison.OrdinalIgnoreCase)) continue;
            files[Path.GetRelativePath(directory, path)] = File.ReadAllBytes(path);
            File.Delete(path);
        }
        return files;
    }

    static bool SameFiles(SortedDictionary<string, byte[]> a, SortedDictionary<string, byte[]> b)
    {
        return a.Count == b.Count && a.All(kv => b.TryGetValue(kv.Key, out var v) && v.AsSpan().SequenceEqual(kv.Value));
    }

    static string Quote(string s)
    {
        return "'" + s.Replace("\\", "\\\\").Replace("\n", "\\n").Replace("'", "\\'") + "'";
    }

    public static int Compare(string lame, string ctLame, bool keep)
    {
        string work = Path.Combine(Path.GetTempPath(), "ctlame-cli-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        string lameDir = Path.Combine(work, "lame"), ctLameDir = Path.Combine(work, "ctlame");
        foreach (var d in new[] { lameDir, ctLameDir }) { Directory.CreateDirectory(d); MakeInputs(d); }

        var cases = BuildCases();
        int failures = 0, known = 0, mp3Files = 0;
        for (int index = 0; index < cases.Count; index++)
        {
            var c = cases[index];
            uint eCode, aCode; string eOut, eErr, aOut, aErr;
            Run(lame, c.Args, c.EnvOpt, lameDir, out eCode, out eOut, out eErr);
            Run(ctLame, c.Args, c.EnvOpt, ctLameDir, out aCode, out aOut, out aErr);
            var eFiles = TakeMp3Files(lameDir);
            var aFiles = TakeMp3Files(ctLameDir);
            aOut = AsLameText(aOut); aErr = AsLameText(aErr);
            mp3Files += eFiles.Count;
            bool same = eCode == aCode && eOut == aOut && eErr == aErr && SameFiles(eFiles, aFiles);
            string key = string.Join(" ", c.Args);
            if (!same && c.EnvOpt == null && KnownDifferences.ContainsKey(key))
            {
                known++;
                Console.WriteLine("--- case " + index + ": " + key + ": expected difference, " + KnownDifferences[key]);
                continue;
            }
            if (same) continue;
            failures++;
            Console.WriteLine("--- case " + index + ": " + key + (c.EnvOpt != null ? "   [LAMEOPT=" + c.EnvOpt + "]" : ""));
            if (eCode != aCode) Console.WriteLine("  exit code differs:\n    lame : " + eCode + "\n    ctLame: " + aCode);
            if (eOut != aOut) Console.WriteLine("  stdout differs:\n    lame : " + Quote(eOut) + "\n    ctLame: " + Quote(aOut));
            if (eErr != aErr) Console.WriteLine("  stderr differs:\n    lame : " + Quote(eErr) + "\n    ctLame: " + Quote(aErr));
            foreach (var name in eFiles.Keys.Union(aFiles.Keys).OrderBy(n => n, StringComparer.Ordinal))
            {
                byte[] e, a;
                eFiles.TryGetValue(name, out e); aFiles.TryGetValue(name, out a);
                if (e != null && a != null && e.AsSpan().SequenceEqual(a)) continue;
                Func<byte[], string> describe = d => d == null ? "missing" : d.Length + " bytes";
                Console.WriteLine("  " + name + " differs: lame " + describe(e) + ", ctLame " + describe(a));
            }
        }

        Console.WriteLine((cases.Count - failures - known) + " of " + cases.Count + " cases identical, " + known + " expected differences, " + failures + " failures");
        Console.WriteLine(mp3Files + " MP3 files of lame compared");
        if (keep) Console.WriteLine("work directory: " + work);
        else try { Directory.Delete(work, true); } catch (IOException) { }
        return failures > 0 ? 1 : 0;
    }
}
'@

$exitCode = [CtLameCliCompare]::Compare((Resolve-Path -LiteralPath $Lame).Path, (Resolve-Path -LiteralPath $CtLame).Path, $Keep.IsPresent)
exit $exitCode
