ctOpus benchmark
================

Runs ctOpus against opusenc (opus-tools 0.2, libopusenc 0.2.1, libopus 1.6.1) on a folder of
WAV files. Measures the wall time per file, the throughput (x real time) and the speedup, and
checks that ctOpus writes exactly the same file as opusenc.

Requirements: Windows 10 or 11 (x64), PowerShell 5.1 or later. No installation.

Usage:
  run.cmd "D:\Music"
  powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 "D:\Music"

Options:
  -EncodeArgs "..."     settings for both programs (default "--comp 10 --bitrate 128")
  -ReferenceArgs "..."  other settings for opusenc, e.g. "--comp 0" (identity is still checked
                        against opusenc with the ctOpus settings)
  -Sequential           also time ctOpus --sequential (one thread)
  -Threads n            ctOpus --threads n
  -Rounds n             n runs per file and program, the median counts
  -Files Quick|n        every 17th file, or the first n files
  -Recurse              include subfolders
  -KeepPowerPlan        do not switch to High Performance

Power plan: for the run the script switches to High Performance (Ultimate Performance if that is
the only one; where the plan is hidden, a temporary copy that is deleted afterwards), checks that
the switch took effect, and restores the previous plan at the end, also after an error or Ctrl+C.
If Windows refuses the switch (e.g. Windows Server without administrator rights, or a group
policy), the report says so ("power plan note") and the run goes on with the current plan.
-KeepPowerPlan leaves the plan alone.

The report (.txt) and the numbers per file (.csv) go to results\. Exit codes: 0 every file was
encoded and is bit-identical to opusenc; 2 at least one file differs or was not encoded; 1 other
errors (missing programs, bad parameters, abort).
