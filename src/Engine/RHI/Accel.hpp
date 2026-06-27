#pragma once

#include "Engine/RHI/Resource.hpp"
#include "Engine/RHI/Heaps.hpp"

namespace pulse::rhi {

class Device;

// DXR acceleration-structure manager (inline RayQuery only, no shader tables /
// DispatchRays). One static opaque-triangle BLAS per mesh, built once; one TLAS
// rebuilt in place each frame from CPU-authored instance descs. The TLAS is
// exposed through a bindless SRV so any cs_6_6 / ps_6_6 shader can index it as a
// RaytracingAccelerationStructure. Requires DXR (caller checks caps().isRt()).
class RtAccel {
public:
    void init(Device* device, Heaps* heaps);

    // Build a static opaque triangle BLAS. Positions are read from vb at offset 0
    // (R32G32B32, stride vertexStride); indices from ib (R32_UINT). Records the
    // build + a UAV barrier onto cmd; scratchOut must outlive the GPU build (the
    // caller releases it after its flush). Returns the BLAS buffer (resting in
    // RAYTRACING_ACCELERATION_STRUCTURE state).
    Buffer buildBlas(ID3D12GraphicsCommandList4* cmd,
                     const Buffer& vb, uint32_t vertexCount, uint32_t vertexStride,
                     const Buffer& ib, uint32_t indexCount, Buffer& scratchOut);

    // Rebuild the TLAS in place from `count` instance descs. Grows the result /
    // scratch / upload buffers and (re)creates the bindless SRV as needed, uploads
    // the descs, and records the build + a UAV barrier onto cmd. No-op if count==0.
    void buildTlas(ID3D12GraphicsCommandList4* cmd,
                   const D3D12_RAYTRACING_INSTANCE_DESC* instances, uint32_t count);

    uint32_t tlasSrv() const { return tlasSrv_; }     // bindless SRV index (0 if none yet)
    uint32_t instanceCount() const { return tlasCount_; }

private:
    void ensureCapacity(uint32_t count);

    Device*               device_ = nullptr;
    Heaps*                heaps_  = nullptr;
    ComPtr<ID3D12Device5> dev5_;

    Buffer   tlasResult_;
    Buffer   tlasScratch_;
    Buffer   tlasUpload_;     // UPLOAD heap, holds the instance descs
    uint32_t tlasCapacity_ = 0;
    uint32_t tlasCount_    = 0;
    uint32_t tlasSrv_      = 0;
};

} // namespace pulse::rhi
