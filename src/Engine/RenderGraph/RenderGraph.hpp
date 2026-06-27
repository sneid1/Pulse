#pragma once

#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Heaps.hpp"
#include "Engine/RHI/Resource.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::render {

using namespace pulse::rhi;

struct RGHandle {
    uint32_t id = UINT32_MAX;
    bool valid() const { return id != UINT32_MAX; }
    bool operator==(const RGHandle& o) const { return id == o.id; }
};

struct RGTextureDesc {
    uint32_t    width  = 0;
    uint32_t    height = 0;
    uint32_t    depth  = 1;     // > 1 -> a 3D texture (froxel volume)
    uint32_t    mips   = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    TextureUsage usage = TextureUsage::None;
    bool        clear  = false;
    float       clearColor[4] = { 0, 0, 0, 1 };
    float       clearDepth    = 0.0f;            // reverse-Z: clear depth to 0
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;  // for depth resources
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;  // override SRV format (e.g. depth-as-R32F)
};

// How a pass touches a resource, mapped to a D3D12 resource state by the graph,
// which inserts the transition barriers. The pass author never writes a barrier.
enum class RGAccess : uint8_t {
    SampledPixel, SampledCompute, RenderTarget, DepthWrite, DepthRead, Uav, CopySrc, CopyDst, Present
};

class RenderGraph;

// Setup phase: declare dependencies only.
class PassBuilder {
public:
    void read (RGHandle h, RGAccess a = RGAccess::SampledPixel);
    void write(RGHandle h, RGAccess a = RGAccess::RenderTarget);

    // Convenience.
    void sample(RGHandle h)        { read(h, RGAccess::SampledPixel); }
    void sampleCompute(RGHandle h) { read(h, RGAccess::SampledCompute); }
    void renderTarget(RGHandle h)  { write(h, RGAccess::RenderTarget); }
    void depthTarget(RGHandle h)   { write(h, RGAccess::DepthWrite); }
    void uav(RGHandle h)           { write(h, RGAccess::Uav); }

private:
    friend class RenderGraph;
    struct Access { uint32_t resource; RGAccess access; bool isWrite; };
    std::vector<Access> accesses_;
};

// Execute phase: record GPU work. Render targets / depth are already bound by the
// graph for raster passes; the lambda sets a PSO, pushes constants, and draws.
struct PassContext {
    ID3D12GraphicsCommandList* cmd = nullptr;
    RenderGraph* graph = nullptr;
    uint32_t width = 0, height = 0;     // bound target dimensions (raster passes)
    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    bool     hasDepth = false;          // a depth target is bound this pass

    // Bindless SRV index for a graph resource (lazily created). Pass it to a
    // shader via root constants; the shader reads ResourceDescriptorHeap[index].
    uint32_t srv(RGHandle h);
    // Bindless UAV index for a graph resource (lazily created); for compute writes.
    uint32_t uav(RGHandle h);
    Texture* texture(RGHandle h);

    // Re-clear the bound depth target mid-pass (e.g. before a first-person
    // viewmodel so it always renders on top of the world).
    void clearDepth(float value = 0.0f) {
        if (hasDepth) cmd->ClearDepthStencilView(depthDsv, D3D12_CLEAR_FLAG_DEPTH, value, 0, 0, nullptr);
    }

    void graphicsConstants(const void* data, uint32_t numDwords, uint32_t offset = 0) {
        cmd->SetGraphicsRoot32BitConstants(0, numDwords, data, offset);
    }
    void computeConstants(const void* data, uint32_t numDwords, uint32_t offset = 0) {
        cmd->SetComputeRoot32BitConstants(0, numDwords, data, offset);
    }
};

class RenderGraph {
public:
    void init(Device* device, Heaps* heaps, ID3D12RootSignature* rootSig);
    void reset();   // start a new frame: drop passes + transient resources
    // Release cached GPU objects/views, used when persistent render targets are
    // about to be destroyed during a window resize. Descriptor slots are kept.
    void releaseResources();

    RGHandle createTexture(const char* name, const RGTextureDesc& desc);
    // Import an external texture (e.g. the LDR/backbuffer). dims/format are taken
    // from the texture; clear lets the first writing pass clear it.
    RGHandle importTexture(const char* name, Texture* external, D3D12_RESOURCE_STATES state,
                           bool clear = false, const float* clearColor = nullptr);
    // Import a persistent depth target (carries dsv/srv formats + depth clear), so
    // large fixed targets like the shadow map stay out of the transient alias pool.
    RGHandle importDepth(const char* name, Texture* external, D3D12_RESOURCE_STATES state,
                         bool clear, float clearDepth, DXGI_FORMAT dsvFormat, DXGI_FORMAT srvFormat);

    using SetupFn = std::function<void(PassBuilder&)>;
    using ExecFn  = std::function<void(PassContext&)>;
    void addRasterPass (const char* name, SetupFn setup, ExecFn exec);
    void addComputePass(const char* name, SetupFn setup, ExecFn exec);

    // Cull unused passes + realise transient resources.
    void compile();
    // Record all kept passes into cmd (auto-barriers between them).
    void execute(ID3D12GraphicsCommandList* cmd);

    // --render-pass hook: every graph resource is named, so dumping one is a
    // lookup + readback. Returns null if not present.
    Texture* findTexture(std::string_view name);
    D3D12_RESOURCE_STATES stateOf(std::string_view name);

    Heaps* heaps() const { return heaps_; }

private:
    friend struct PassContext;

    enum class PassType { Raster, Compute };
    struct Pass {
        std::string name;
        PassType type;
        std::vector<PassBuilder::Access> accesses;
        ExecFn exec;
        bool culled = false;
    };
    struct Resource {
        std::string  name;
        RGTextureDesc desc;
        bool         imported = false;
        Texture*     external = nullptr;     // imported
        ID3D12Resource* importedResource = nullptr;   // detect resource swap (resize)
        Texture      owned;                  // transient (committed in M0; pooled across frames)
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool         needed = false;
        bool         touchedThisFrame = false;
        // *Created = the view matches the CURRENT placed resource (reset when the
        // resource is recreated). *Slot = a descriptor slot was ever allocated for
        // this resource and is reused in place on recreation, so a churning transient
        // layout never leaks descriptors (the heaps are non-freeing bump allocators).
        bool         srvCreated = false; bool srvSlot = false; uint32_t srvIndex = 0;
        bool         uavCreated = false; bool uavSlot = false; uint32_t uavIndex = 0;
        bool         rtvCreated = false; bool rtvSlot = false; D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        bool         dsvCreated = false; bool dsvSlot = false; D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        bool         cleared = false;        // RT/DSV cleared once on first write
        // Transient aliasing (placed resources on a shared heap):
        int          firstUse = -1, lastUse = -1;   // non-culled pass index range
        uint64_t     heapOffset = 0;
        int          aliasPrev = -1;                // resource that previously held this memory

        Texture* tex() { return imported ? external : &owned; }
    };

    int findResource(std::string_view name) const;
    uint32_t srvIndexOf(RGHandle h);
    uint32_t uavIndexOf(RGHandle h);
    static D3D12_RESOURCE_STATES stateFor(RGAccess a);
    void transition(ID3D12GraphicsCommandList* cmd, Resource& r, D3D12_RESOURCE_STATES to,
                    std::vector<D3D12_RESOURCE_BARRIER>& batch);
    void realiseTransientsAliased(const std::vector<uint32_t>& transients);
    void realiseTransientsCommitted();

    Device* device_ = nullptr;
    Heaps*  heaps_  = nullptr;
    ID3D12RootSignature* rootSig_ = nullptr;

    std::vector<Resource> resources_;
    std::vector<Pass>     passes_;
    bool compiled_ = false;
    int  lastKept_ = -1;
    int  lastCulled_ = -1;

    // Transient aliasing: one shared placed-resource heap (heap tier 2), reused
    // across frames while the transient layout is unchanged.
    ComPtr<ID3D12Heap> transientHeap_;
    uint64_t           transientHeapSize_ = 0;
    uint64_t           transientLayoutSig_ = 0;
    bool               aliasingActive_ = false;
    std::vector<uint32_t> transients_;     // resource indices placed on the heap
};

} // namespace pulse::render
