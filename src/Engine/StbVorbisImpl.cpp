// Isolated translation unit for the stb_vorbis implementation. Kept separate from
// Audio.cpp so stb_vorbis's internal single-letter macros (L, R, C, ...) do not
// leak into engine code. Warnings are silenced (third-party, public-domain).

#pragma warning(push, 0)
#include "stb_vorbis.c"
#pragma warning(pop)
