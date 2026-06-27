// Isolated translation unit for the cgltf implementation (glTF 2.0 / GLB loader).
// Kept separate from MeshFile.cpp so the implementation's internal definitions stay
// contained. Warnings silenced (third-party, MIT).

#pragma warning(push, 0)
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#pragma warning(pop)
