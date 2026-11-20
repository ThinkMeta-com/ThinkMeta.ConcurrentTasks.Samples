ctJxl benchmark
===============

Runs ctJxl against cjxl (libjxl 0.13.0, its own thread pool) on a folder of images. Measures the
wall time per image and case, the speedup, and checks that ctJxl writes exactly the same file as
cjxl on its AVX2 target. ctJxl is preliminary (version 0.1.0).

Requirements: Windows 10 or 11, or Windows Server 2016 and later (x64), a CPU with AVX2 (see
CPU below), PowerShell 5.1 or later.
No installation; all programs link the C runtime statically.

Images: any folder of PPM, PGM, PNM, PAM, PFM, PNG, APNG, GIF or JPEG files. After
verify\Prepare-Inputs.ps1, testdata\conformance holds the 25 decoded reference images of the JPEG
XL conformance test set (github.com/libjxl/conformance): alpha, 16 bit, float, grayscale, animation
(APNG, GIF), layers, patches, noise; from 200x200 to 4064x2704. ctJxl's work on one image shows
most on large photos. For animations the report shows the megapixels of one frame.
JPEG input: cjxl recompresses a JPEG losslessly by default ("-d 0" or no options); "-q" below 100
is an error there. To re-encode the pixels lossily, add "-j 0" to the case, e.g.
-Cases "-j 0 -q 90". The default cases therefore fail on a folder with JPEGs.

Usage:
  run.cmd ..\testdata\conformance
  run.cmd D:\Images
  powershell -ExecutionPolicy Bypass -File Compare-Folder.ps1 "D:\Images"

Options:
  -Cases "a;b"          cjxl options per case, separated by ";", the same for both programs
                        (default "-q 90;-d 0"). Example: -Cases "-q 50;-q 90 -e 9;-m 1 -q 80".
                        Efforts 9 and 10 are slow on large photos (minutes per run with cjxl).

  -Threads n            --num_threads n for both programs (default: all logical processors)
  -Rounds n             runs per image, case and program (default 5); the time is the mean of
                        the middle runs (best and worst dropped), with fewer than 3 the median
  -Files Quick|n        every 5th image, or the first n images
  -Recurse              include subfolders
  -KeepJxl              keep the JXL files in work\
  -KeepPowerPlan        do not switch to High Performance
  -Warm                 read the images from the file cache (default: cold, see below)

File cache: the wall time includes reading the image, so every program reads it cold, as a photo
usually is: before each timed run the script writes a fresh copy of the image with unbuffered I/O
(robocopy /J) into work\cold, which is then not in the file cache; the copy is not timed. The
programs therefore read from the disk this folder is on. -Warm reads the images from the cache
(repeated runs of the same file) instead.

Power plan: for the run the script switches to High Performance (Ultimate Performance if that is
the only one; where the plan is hidden, a temporary copy that is deleted afterwards), checks that
the switch took effect, and restores the previous plan at the end, also after an error or Ctrl+C.
If Windows refuses the switch (e.g. Windows Server without administrator rights, or a group
policy), the report says so ("power plan note") and the run goes on with the current plan.
-KeepPowerPlan leaves the plan alone.

Memory: ctJxl trades memory for speed (several DC groups and frames at once): with -q 90 on a
24 MP photo about 710 MB peak against cjxl's 480 MB, an APNG -d 0 530 MB against 220 MB. The
script measures time only.

The report (.txt) and the numbers per image (.csv) go to results\.

Exit codes (of Compare-Folder.ps1 and run.cmd, the same in all ThinkMeta.ConcurrentTasks sample
packages):
  0  every encoding bit-identical to cjxl (every run is compared)
  2  at least one encoding differs, or a program failed on it
  1  other errors: a program missing in ..\bin\win-x64\ (reported before the power plan is touched), wrong
     parameters, no images in the folder, abort

Programs (..\bin\win-x64\):
  ctJxl.exe                          ctJxl 0.1.0 (libjxl 0.13.0 on ThinkMeta.ConcurrentTasks)
  ThinkMeta.ConcurrentTasks.Core.dll the scheduler core of ctJxl
  cjxl.exe                           cjxl 0.13.0, unmodified libjxl, built with the same compiler
                                     (MSVC); the script runs it with --target avx2

Builds: ctJxl.exe is built with /arch:AVX2, whole program optimization and profile-guided
optimization (see the README); cjxl.exe is libjxl's own CMake build, unchanged, without them,
because it is the reference for the bytes. A cjxl with whole program optimization and PGO (but
without /arch:AVX2, which changes its bytes) was mostly 3-6 % faster, at "-q 50" 7 % slower;
/arch:AVX2 alone makes lossy encoding about 13-15 % faster. The factors in the report are against
the unchanged reference.

CPU: ctJxl is built for AVX2 (/arch:AVX2, with FMA and BMI1/2: Intel Haswell, AMD Excavator/Zen
or later) and reproduces libjxl's AVX2 target byte for byte; on other CPUs it stops with exit
code 3 and the script ends with an error.

Licenses (in the folder above): LICENSE.txt (ctJxl's own code, MIT), LICENSE-Core.txt (the scheduler core,
proprietary, non-commercial use, benchmarks explicitly allowed), THIRD-PARTY-NOTICES.txt
(libjxl and the libraries it uses). JPEG input: this software is based in part on the work of the
Independent JPEG Group (libjpeg-turbo).
