#include "Engine/RHI/Accel.hpp"
#include "Engine/RHI/Device.hpp"

#include <cstring>

namespace pulse::rhi {

namespace {

// Acceleration structures (and their scratch) must be DEFAULT buffers created
// with ALLOW_UNORDERED_ACCESS. Unlike a normal buffer, an AS result buffer is
// REQUIRED to be created directly in the RAYTRACING_ACCELERATION_STRUCTURE state
// (you cannot transition into it), so this does not go through createDefaultBuffer
// (which forces COMMON).
Buffer createAsBuffer(ID3D12Device* device, uint64_t size,
                      D3D12_RESOURCE_STATES state, const wchar_t* name) {
    Buffer b;
    b.size = size;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC desc =
        CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    PULSE_HR(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        state, nullptr, IID_PPV_ARGS(&b.resource)));
    if (name) b.resource->SetName(name);
    b.gpuAddress = b.resource->GetGPUVirtualAddress();
    return b;
}

} // namespace

void RtAccel::init(Device* device, Heaps* heaps) {
    device_ = device;
    heaps_  = heaps;
    PULSE_HR(device_->d3d()->QueryInterface(IID_PPV_ARGS(&dev5_)));
}

Buffer RtAccel::buildBlas(ID3D12GraphicsCommandList4* cmd,
                          const Buffer& vb, uint32_t vertexCount, uint32_t vertexStride,
                          const Buffer& ib, uint32_t indexCount, Buffer& scratchOut) {
    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geo.Triangles.Transform3x4 = 0;
    geo.Triangles.IndexFormat  = DXGI_FORMAT_R32_UINT;
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geo.Triangles.IndexCount   = indexCount;
    geo.Triangles.VertexCount  = vertexCount;
    geo.Triangles.IndexBuffer  = ib.gpuAddress;
    geo.Triangles.VertexBuffer.StartAddress  = vb.gpuAddress;   // position is at offset 0
    geo.Triangles.VertexBuffer.StrideInBytes = vertexStride;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs    = 1;
    inputs.pGeometryDescs = &geo;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
    dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &pre);

    Buffer blas = createAsBuffer(device_->d3d(), pre.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"mesh.blas");
    // Scratch: D3D12 forces buffers to COMMON at creation (and warns otherwise);
    // it promotes to UNORDERED_ACCESS implicitly on first use by the build.
    scratchOut = createAsBuffer(device_->d3d(), pre.ScratchDataSizeInBytes,
        D3D12_RESOURCE_STATE_COMMON, L"mesh.blas.scratch");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData    = blas.gpuAddress;
    build.ScratchAccelerationStructureData = scratchOut.gpuAddress;
    cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(blas.get());
    cmd->ResourceBarrier(1, &uav);
    return blas;
}

void RtAccel::ensureCapacity(uint32_t count) {
    if (tlasResult_.resource && count <= tlasCapacity_) return;
    const uint32_t cap = count < 256 ? 256u : count + count / 2;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs    = cap;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
    dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &pre);

    tlasResult_  = createAsBuffer(device_->d3d(), pre.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"pulse.tlas");
    tlasScratch_ = createAsBuffer(device_->d3d(), pre.ScratchDataSizeInBytes,
        D3D12_RESOURCE_STATE_COMMON, L"pulse.tlas.scratch");
    tlasUpload_  = createUploadBuffer(device_->d3d(),
        sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * cap, L"pulse.tlas.upload");
    tlasCapacity_ = cap;
    tlasSrv_ = heaps_->createTlasSrv(tlasResult_.gpuAddress);   // re-created on growth
}

void RtAccel::buildTlas(ID3D12GraphicsCommandList4* cmd,
                        const D3D12_RAYTRACING_INSTANCE_DESC* instances, uint32_t count) {
    tlasCount_ = count;
    if (count == 0) return;
    ensureCapacity(count);
    std::memcpy(tlasUpload_.mapped, instances, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * count);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs      = count;
    inputs.InstanceDescs = tlasUpload_.gpuAddress;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData    = tlasResult_.gpuAddress;
    build.ScratchAccelerationStructureData = tlasScratch_.gpuAddress;
    cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(tlasResult_.get());
    cmd->ResourceBarrier(1, &uav);
}

} // namespace pulse::rhi
