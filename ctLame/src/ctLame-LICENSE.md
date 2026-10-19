# ctLame license

The ctLame projects in this folder (`ThinkMeta.ConcurrentTasks.Samples.ctLame*`) are a C++ port of
the LAME 4.0 MP3 encoder, reduced to MPEG-1 Layer III VBR (see `../lame/`). As a derived work of
LAME they are distributed under the GNU Library General Public License, version 2 or (at your
option) any later version, like LAME itself (`../lame/COPYING`, `../lame/LICENSE`).

Copyright (c) 1999-2011 The LAME Project, and the authors named in the LAME sources.
Copyright (c) 2026 ThinkMeta Software GmbH, for the port.

ctLame does not link LAME. It is a port that produces the same output; only constant tables are
taken over from LAME as data and are marked as such in the source (`MdctTables.inc`,
`HuffmanTables.inc` and the tables in `PsyInit.cpp`, `PsyPrepare.cpp`, `PsyResolve.cpp`,
`BitCount.cpp`, `VbrQuantize.cpp`).
