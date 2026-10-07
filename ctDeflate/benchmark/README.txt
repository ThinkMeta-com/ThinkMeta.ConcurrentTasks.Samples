ctDeflate benchmark against libdeflate 1.26 gzip.exe
====================================================

What it does
  Compresses every input file at every chosen level with gzip.exe (the official libdeflate 1.26
  build, the reference) and with ctDeflate, one program at a time, alternating per round. It
  measures the wall time of each run (start, reading, compressing, writing) and checks that
  ctDeflate's .gz is byte for byte the same as gzip.exe's. Each input is first copied to work\,
  so both programs read the same local copy.

How to run
  - Windows PowerShell 5.1 or later (also Windows Server 2016); no installation.
  - Close other busy programs; on a notebook plug in the power supply. The script switches to
    the High Performance power plan for the run and restores the previous one afterwards, also
    after an error or Ctrl+C; if Windows refuses (Windows Server without administrator rights,
    group policy), the report says so and the run goes on: then start it as administrator or
    choose the plan by hand.
  - Double-click run.cmd and enter a file or folder, or on the command line:
        run.cmd D:\Data\big.bin D:\Logs
        run.cmd D:\Logs -Recurse -Levels 1,6,9 -Rounds 5
  - The report is in results\bench-<computer>-<time>.txt, the numbers in the .csv. The report
    ends with whether the previous power plan was restored.
  - Exit code of the script and of run.cmd: 0 if every output was identical, 2 if at least one
    output differed or a run failed, 1 for other errors (a missing program, wrong parameters,
    ctDeflate refusing the CPU - it needs AVX2, FMA, BMI2 and PCLMUL - or an abort). Missing
    programs, the CPU and the levels (1 to 9) are checked before the power plan is switched.

Options
  -Levels 1,6,9        compression levels (default 1,6,9)
  -Rounds 3            runs per file, level and program, the median counts (default 3)
  -Threads 8           ctDeflate with 8 threads (default: all logical processors)
  -CtArgs "--in-memory"  further ctDeflate arguments
  -Recurse             also the files in subfolders
  -GzipExe, -CtDeflateExe  the programs (default: bin\ next to the script or one level up)
  -KeepPowerPlan       do not switch the power plan

Notes
  - gzip.exe compresses on one thread; ctDeflate on all logical processors unless -Threads.
  - The times include starting the program and the file I/O, so small files (a few MB) favour
    gzip.exe; ctDeflate splits the input into 1 MiB chunks and gains most on large files.
  - work\ needs room for one input copy and two outputs at a time.
  - Without file arguments the script reads Bench-Files.txt next to it (one path per line) if
    present, else it asks for a file or folder.
