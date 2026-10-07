ctJpeg benchmark
================

Runs ctJpeg against jpegli (cjpegli, google/jpegli @ 031a007) on a folder of PPM/PGM images, and
for comparison mozjpeg 4.1.5 and libjpeg-turbo 3.2.0. Measures the wall time per image, the
throughput (megapixels per second) and the speedup, and checks that ctJpeg writes exactly the
same file as jpegli on the chosen feature set.

Requirements: Windows 10 or 11 (x64), PowerShell 5.1 or later. No installation.

Input: binary PPM (P6, 8-bit RGB) or PGM (P5, 8-bit gray). ctJpeg reads no other format. To
convert JPEG or PNG files, for example with ImageMagick:
  magick mogrify -format ppm *.png

Usage:
  run.cmd "D:\Images"
  powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 "D:\Images"

Options:
  -Quality n            quality for all programs (default 85)
  -Simd auto|sse2|avx2  ctJpeg feature set; jpegli runs on the same SIMD target (default: avx2
                        if the CPU has it). jpegli writes different bytes on SSE and AVX2, and
                        ctJpeg reproduces each.
  -Sequential           also time ctJpeg --sequential (one thread)
  -Threads n            ctJpeg --threads n
  -NoOthers             skip mozjpeg and libjpeg-turbo
  -Rounds n             n runs per image and program, the median counts
  -Files Quick|n        every 5th image, or the first n images
  -Recurse              include subfolders
  -KeepJpeg             keep the JPEG files in work\
  -KeepPowerPlan        do not switch to High Performance

Power plan: for the run the script switches to High Performance (Ultimate Performance if that is
the only one; where the plan is hidden, a temporary copy that is deleted afterwards), checks that
the switch took effect, and restores the previous plan at the end, also after an error or Ctrl+C.
If Windows refuses the switch (e.g. Windows Server without administrator rights, or a group
policy), the report says so ("power plan note") and the run goes on with the current plan.
-KeepPowerPlan leaves the plan alone.

The report (.txt) and the numbers per image (.csv) go to results\.

Exit codes (of Compare-Folder.ps1 and run.cmd, the same in all ThinkMeta.ConcurrentTasks sample
packages):
  0  every image bit-identical to jpegli
  2  at least one image differs, or a program failed on it
  1  other errors: a program missing in bin\ (reported before the power plan is touched), wrong
     parameters, no images in the folder, abort
