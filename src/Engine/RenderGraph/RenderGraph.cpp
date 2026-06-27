#include "Engine/RenderGraph/RenderGraph.hpp"

#include <algorithm>

namespace pulse::render {

// ----------------------------------------------------------------- PassBuilder
void PassBuilder::read(RGHandle h, RGAccess a)  { accesses_.push_back({ h.id, a, false }); }
void PassBuilder::write(RGHandle h, RGAccess a) { accesses_.push_back({ h.id, a, true  }); }

// ---------------------------------------------------------------- PassContext
uint32_t PassContext::srv(RGHandle h) { return graph->srvIndexOf(h); }
uint32_t PassContext::uav(RGHandle h) { return graph->uavIndexOf(h); }
Texture* PassContext::texture(RGHandle h) { return graph->resources_[h.id].tex(); }

// ----------------------------------------------------------------- RenderGraph
void RenderGraph::init(Device* device, Heaps* heaps, ID3D12RootSignature* rootSig) {
    device_ = device;
    heaps_ = heaps;
    rootSig_ = rootSig;
}

void RenderGraph::reset() {
    passes_.clear();
    // Resources persist across frames (the transient pool); only per-frame flags
    // reset. Owned resources keep their tracked GPU state between frames.
    for (auto& r : resources_) {
        r.needed = false;
        r.cleared = false;
        r.touchedThisFrame = false;
    }
    compiled_ = false;
}

void RenderGraph::releaseResources() {
    if (device_) device_->flushGraphics();
    passes_.clear();
    for (auto& r : resources_) {
        if (r.imported) {
            r.external = nullptr;
            r.importedResource = nullptr;
        } else {
            r.owned = Texture{};
        }
        r.needed = false;
        r.touchedThisFrame = false;
        r.srvCreated = false;
        r.uavCreated = false;
        r.rtvCreated = false;
        r.dsvCreated = false;
        r.cleared = false;
        r.firstUse = -1;
        r.lastUse = -1;
        r.heapOffset = 0;
        r.aliasPrev = -1;
        r.state = D3D12_RESOURCE_STATE_COMMON;
    }
    transients_.clear();
    transientHeap_.Reset();
    transientHeapSize_ = 0;
    transientLayoutSig_ = 0;
    aliasingActive_ = false;
    compiled_ = false;
    lastKept_ = -1;
    lastCulled_ = -1;
}

int RenderGraph::findResource(std::string_view name) const {
    for (size_t i = 0; i < resources_.size(); ++i)
        if (resources_[i].name == name) return static_cast<int>(i);
    return -1;
}

RGHandle RenderGraph::createTexture(const char* name, const RGTextureDesc& desc) {
    const int existing = findResource(name);
    if (existing >= 0) {
        Resource& r = resources_[existing];
        const bool sameShape = !r.imported && r.desc.width == desc.width &&
            r.desc.height == desc.height && r.desc.format == desc.format &&
            static_cast<uint32_t>(r.desc.usage) == static_cast<uint32_t>(desc.usage);
        r.desc = desc;
        r.touchedThisFrame = true;
        if (!sameShape) {           // resized/reformatted: drop the old texture + views
            r.owned = Texture{};
            r.srvCreated = r.rtvCreated = r.dsvCreated = r.uavCreated = false;
        }
        return { static_cast<uint32_t>(existing) };
    }
    Resource r;
    r.name = name;
    r.desc = desc;
    r.imported = false;
    r.touchedThisFrame = true;
    resources_.push_back(std::move(r));
    return { static_cast<uint32_t>(resources_.size() - 1) };
}

RGHandle RenderGraph::importTexture(const char* name, Texture* external, D3D12_RESOURCE_STATES state,
                                    bool clear, const float* clearColor) {
    auto fillDesc = [&](Resource& r) {
        r.desc.width = external->width;
        r.desc.height = external->height;
        r.desc.format = external->format;
        r.desc.mips = external->mips ? external->mips : 1;
        r.desc.clear = clear;
        if (clearColor) for (int i = 0; i < 4; ++i) r.desc.clearColor[i] = clearColor[i];
    };
    const int existing = findResource(name);
    if (existing >= 0) {
        Resource& r = resources_[existing];
        // Compare the underlying D3D resource, not just the Texture* (resize swaps
        // the resource behind the same Texture object). Invalidate cached views.
        ID3D12Resource* res = external ? external->get() : nullptr;
        if (r.importedResource != res) {
            r.srvCreated = r.rtvCreated = r.dsvCreated = false;
            r.importedResource = res;
        }
        r.external = external;
        r.state = state;
        r.imported = true;
        r.touchedThisFrame = true;
        fillDesc(r);
        return { static_cast<uint32_t>(existing) };
    }
    Resource r;
    r.name = name;
    r.imported = true;
    r.external = external;
    r.importedResource = external ? external->get() : nullptr;
    r.state = state;
    r.touchedThisFrame = true;
    fillDesc(r);
    resources_.push_back(std::move(r));
    return { static_cast<uint32_t>(resources_.size() - 1) };
}

RGHandle RenderGraph::importDepth(const char* name, Texture* external, D3D12_RESOURCE_STATES state,
                                  bool clear, float clearDepth, DXGI_FORMAT dsvFormat, DXGI_FORMAT srvFormat) {
    auto fillDesc = [&](Resource& r) {
        r.desc.width = external->width;
        r.desc.height = external->height;
        r.desc.format = external->format;
        r.desc.mips = 1;
        r.desc.clear = clear;
        r.desc.clearDepth = clearDepth;
        r.desc.dsvFormat = dsvFormat;
        r.desc.srvFormat = srvFormat;
        r.desc.usage = TextureUsage::DepthStencil;
    };
    const int existing = findResource(name);
    if (existing >= 0) {
        Resource& r = resources_[existing];
        ID3D12Resource* res = external ? external->get() : nullptr;
        if (r.importedResource != res) {
            r.srvCreated = r.rtvCreated = r.dsvCreated = false;
            r.importedResource = res;
        }
        r.external = external; r.state = state; r.imported = true; r.touchedThisFrame = true;
        fillDesc(r);
        return { static_cast<uint32_t>(existing) };
    }
    Resource r;
    r.name = name; r.imported = true; r.external = external;
    r.importedResource = external ? external->get() : nullptr;
    r.state = state; r.touchedThisFrame = true;
    fillDesc(r);
    resources_.push_back(std::move(r));
    return { static_cast<uint32_t>(resources_.size() - 1) };
}

void RenderGraph::addRasterPass(const char* name, SetupFn setup, ExecFn exec) {
    PassBuilder b; setup(b);
    passes_.push_back({ name, PassType::Raster, std::move(b.accesses_), std::move(exec), false });
}
void RenderGraph::addComputePass(const char* name, SetupFn setup, ExecFn exec) {
    PassBuilder b; setup(b);
    passes_.push_back({ name, PassType::Compute, std::move(b.accesses_), std::move(exec), false });
}

void RenderGraph::compile() {
    // --- cull: keep only passes that contribute to an imported (output) resource.
    // A pass is needed if it writes a needed resource; ALL of a needed pass's
    // resources (reads and writes, e.g. its depth target) then become needed too.
    for (auto& r : resources_) r.needed = r.imported && r.touchedThisFrame;

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& p : passes_) {
            bool passNeeded = false;
            for (const auto& a : p.accesses)
                if (a.isWrite && resources_[a.resource].needed) { passNeeded = true; break; }
            if (!passNeeded) continue;
            for (const auto& a : p.accesses) {
                if (!resources_[a.resource].needed) {
                    resources_[a.resource].needed = true;
                    changed = true;
                }
            }
        }
    }
    for (auto& p : passes_) {
        p.culled = true;
        for (const auto& a : p.accesses)
            if (a.isWrite && resources_[a.resource].needed) { p.culled = false; break; }
    }

    // --- transient lifetimes (non-culled pass index range per resource).
    for (auto& r : resources_) { r.firstUse = -1; r.lastUse = -1; }
    for (int p = 0; p < static_cast<int>(passes_.size()); ++p) {
        if (passes_[p].culled) continue;
        for (const auto& a : passes_[p].accesses) {
            Resource& r = resources_[a.resource];
            if (r.firstUse < 0) r.firstUse = p;
            r.lastUse = p;
        }
    }
    transients_.clear();
    for (uint32_t i = 0; i < resources_.size(); ++i)
        if (!resources_[i].imported && resources_[i].needed) transients_.push_back(i);

    // --- realise transients: placed + aliased on a shared heap (heap tier 2),
    // else committed.
    aliasingActive_ = false;
    if (device_->caps().heapTier >= D3D12_RESOURCE_HEAP_TIER_2 && !transients_.empty())
        realiseTransientsAliased(transients_);
    else
        realiseTransientsCommitted();

    int kept = 0, culled = 0;
    for (const auto& p : passes_) (p.culled ? culled : kept)++;
    if (kept != lastKept_ || culled != lastCulled_) {       // only log when it changes
        logInfo("render graph: %d pass(es) kept, %d culled", kept, culled);
        lastKept_ = kept; lastCulled_ = culled;
    }
    compiled_ = true;
}

namespace {

D3D12_RESOURCE_DESC transientResourceDesc(const RGTextureDesc& d) {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (hasFlag(d.usage, TextureUsage::RenderTarget))    flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (hasFlag(d.usage, TextureUsage::DepthStencil))    flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (hasFlag(d.usage, TextureUsage::UnorderedAccess)) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (d.depth > 1)
        return CD3DX12_RESOURCE_DESC::Tex3D(d.format, d.width, d.height, static_cast<UINT16>(d.depth),
            static_cast<UINT16>(d.mips ? d.mips : 1), flags);
    return CD3DX12_RESOURCE_DESC::Tex2D(d.format, d.width, d.height, 1,
        static_cast<UINT16>(d.mips ? d.mips : 1), 1, 0, flags);
}

D3D12_RESOURCE_STATES transientInitialState(const RGTextureDesc& d) {
    if (hasFlag(d.usage, TextureUsage::RenderTarget))    return D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (hasFlag(d.usage, TextureUsage::DepthStencil))    return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (hasFlag(d.usage, TextureUsage::UnorderedAccess)) return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return D3D12_RESOURCE_STATE_COMMON;
}

bool transientClearValue(const RGTextureDesc& d, D3D12_CLEAR_VALUE& out) {
    if (hasFlag(d.usage, TextureUsage::RenderTarget)) {
        out.Format = d.format;
        for (int i = 0; i < 4; ++i) out.Color[i] = d.clearColor[i];
        return true;
    }
    if (hasFlag(d.usage, TextureUsage::DepthStencil)) {
        out.Format = (d.dsvFormat != DXGI_FORMAT_UNKNOWN) ? d.dsvFormat
                   : (d.format == DXGI_FORMAT_R32_TYPELESS) ? DXGI_FORMAT_D32_FLOAT : d.format;
        out.DepthStencil.Depth = d.clearDepth;
        out.DepthStencil.Stencil = 0;
        return true;
    }
    return false;
}

} // namespace

void RenderGraph::realiseTransientsCommitted() {
    for (uint32_t idx : transients_) {
        Resource& r = resources_[idx];
        if (r.owned) continue;                          // already realised (pooled)
        const D3D12_RESOURCE_STATES initial = transientInitialState(r.desc);
        const float clear[4] = { r.desc.clearColor[0], r.desc.clearColor[1],
                                 r.desc.clearColor[2], r.desc.clearColor[3] };
        float depthClear[4] = { r.desc.clearDepth, 0, 0, 0 };
        const float* clearPtr = hasFlag(r.desc.usage, TextureUsage::RenderTarget) ? clear
                              : hasFlag(r.desc.usage, TextureUsage::DepthStencil) ? depthClear : nullptr;
        std::wstring wname(r.name.begin(), r.name.end());
        if (r.desc.depth > 1) {
            // 3D froxel volume (committed fallback for non-tier-2 heaps).
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            const D3D12_RESOURCE_DESC desc = transientResourceDesc(r.desc);
            Texture t;
            t.width = r.desc.width; t.height = r.desc.height;
            t.mips = r.desc.mips ? r.desc.mips : 1; t.format = r.desc.format; t.state = initial;
            PULSE_HR(device_->d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                initial, nullptr, IID_PPV_ARGS(&t.resource)));
            t.resource->SetName(wname.c_str());
            r.owned = std::move(t);
            r.state = initial;
            r.srvCreated = r.uavCreated = false;
            continue;
        }
        r.owned = createTexture2D(device_->d3d(), r.desc.width, r.desc.height, r.desc.format,
                                  r.desc.usage, initial, r.desc.mips, clearPtr, wname.c_str());
        r.state = initial;
    }
}

void RenderGraph::realiseTransientsAliased(const std::vector<uint32_t>& transients) {
    aliasingActive_ = true;

    // Greedy interval allocation: process by firstUse, reuse a freed region when a
    // resource's lifetime does not overlap the region's current occupant.
    std::vector<uint32_t> order = transients;
    std::sort(order.begin(), order.end(),
              [&](uint32_t a, uint32_t b) { return resources_[a].firstUse < resources_[b].firstUse; });

    struct Bucket { uint64_t offset, size; int lastUse; int occupant; };
    std::vector<Bucket> buckets;
    uint64_t heapTop = 0;
    for (uint32_t idx : order) {
        Resource& r = resources_[idx];
        const D3D12_RESOURCE_DESC desc = transientResourceDesc(r.desc);
        const D3D12_RESOURCE_ALLOCATION_INFO info = device_->d3d()->GetResourceAllocationInfo(0, 1, &desc);
        const uint64_t size = info.SizeInBytes, align = info.Alignment;

        int chosen = -1;
        for (size_t b = 0; b < buckets.size(); ++b)
            if (buckets[b].lastUse < r.firstUse && buckets[b].size >= size && (buckets[b].offset % align) == 0) {
                chosen = static_cast<int>(b); break;
            }
        if (chosen >= 0) {
            r.heapOffset = buckets[chosen].offset;
            r.aliasPrev = buckets[chosen].occupant;
            buckets[chosen].lastUse = r.lastUse;
            buckets[chosen].occupant = static_cast<int>(idx);
        } else {
            const uint64_t off = (heapTop + align - 1) & ~(align - 1);
            r.heapOffset = off; r.aliasPrev = -1;
            heapTop = off + size;
            buckets.push_back({ off, size, r.lastUse, static_cast<int>(idx) });
        }
    }
    const uint64_t heapSize = heapTop;

    // Signature so identical frames reuse the heap + placed resources (pooled).
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { sig ^= v; sig *= 1099511628211ull; };
    for (uint32_t idx : order) {
        const Resource& r = resources_[idx];
        mix(r.heapOffset); mix(r.desc.width); mix(r.desc.height); mix(static_cast<uint64_t>(r.desc.format));
    }
    mix(heapSize);

    if (sig != transientLayoutSig_ || !transientHeap_) {
        device_->flushGraphics();        // no in-flight use of resources being replaced
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = heapSize;
        hd.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        hd.Flags = D3D12_HEAP_FLAG_NONE;     // heap tier 2: all resource types allowed
        transientHeap_.Reset();
        PULSE_HR(device_->d3d()->CreateHeap(&hd, IID_PPV_ARGS(&transientHeap_)));
        transientHeap_->SetName(L"pulse.transient.heap");
        transientHeapSize_ = heapSize;
        transientLayoutSig_ = sig;

        for (uint32_t idx : order) {
            Resource& r = resources_[idx];
            const D3D12_RESOURCE_DESC desc = transientResourceDesc(r.desc);
            const D3D12_RESOURCE_STATES initial = transientInitialState(r.desc);
            D3D12_CLEAR_VALUE clear{};
            const bool hasClear = transientClearValue(r.desc, clear);
            Texture t;
            t.width = r.desc.width; t.height = r.desc.height;
            t.mips = r.desc.mips ? r.desc.mips : 1; t.format = r.desc.format; t.state = initial;
            PULSE_HR(device_->d3d()->CreatePlacedResource(transientHeap_.Get(), r.heapOffset, &desc,
                initial, hasClear ? &clear : nullptr, IID_PPV_ARGS(&t.resource)));
            std::wstring wname(r.name.begin(), r.name.end());
            t.resource->SetName(wname.c_str());
            r.owned = std::move(t);
            r.state = initial;
            r.srvCreated = r.rtvCreated = r.dsvCreated = r.uavCreated = false;   // placed resource changed
        }
    }
}

D3D12_RESOURCE_STATES RenderGraph::stateFor(RGAccess a) {
    switch (a) {
        case RGAccess::SampledPixel:   return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case RGAccess::SampledCompute: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case RGAccess::RenderTarget:   return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case RGAccess::DepthWrite:     return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case RGAccess::DepthRead:      return D3D12_RESOURCE_STATE_DEPTH_READ;
        case RGAccess::Uav:            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case RGAccess::CopySrc:        return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case RGAccess::CopyDst:        return D3D12_RESOURCE_STATE_COPY_DEST;
        case RGAccess::Present:        return D3D12_RESOURCE_STATE_PRESENT;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

void RenderGraph::transition(ID3D12GraphicsCommandList*, Resource& r, D3D12_RESOURCE_STATES to,
                             std::vector<D3D12_RESOURCE_BARRIER>& batch) {
    if (r.state == to) return;
    batch.push_back(CD3DX12_RESOURCE_BARRIER::Transition(r.tex()->get(), r.state, to));
    r.state = to;
}

uint32_t RenderGraph::srvIndexOf(RGHandle h) {
    Resource& r = resources_[h.id];
    if (!r.srvCreated) {
        const DXGI_FORMAT fmt = r.desc.srvFormat != DXGI_FORMAT_UNKNOWN ? r.desc.srvFormat : r.desc.format;
        const uint32_t mips = r.desc.mips ? r.desc.mips : 1;
        const uint32_t reuse = r.srvSlot ? r.srvIndex : rhi::Heaps::kReuseNone;
        r.srvIndex = (r.desc.depth > 1) ? heaps_->createTexture3DSrv(r.tex()->get(), fmt, mips, reuse)
                                        : heaps_->createTextureSrv(r.tex()->get(), fmt, mips, reuse);
        r.srvSlot = true;
        r.srvCreated = true;
    }
    return r.srvIndex;
}

uint32_t RenderGraph::uavIndexOf(RGHandle h) {
    Resource& r = resources_[h.id];
    if (!r.uavCreated) {
        const uint32_t reuse = r.uavSlot ? r.uavIndex : rhi::Heaps::kReuseNone;
        r.uavIndex = (r.desc.depth > 1)
            ? heaps_->createTexture3DUav(r.tex()->get(), r.desc.format, r.desc.depth, reuse)
            : heaps_->createTextureUav(*r.tex(), reuse);
        r.uavSlot = true;
        r.uavCreated = true;
    }
    return r.uavIndex;
}

void RenderGraph::execute(ID3D12GraphicsCommandList* cmd) {
    PULSE_CHECK(compiled_, "RenderGraph::execute before compile");

    ID3D12DescriptorHeap* heaps[] = { heaps_->bindlessHeap() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(rootSig_);
    cmd->SetComputeRootSignature(rootSig_);

    for (int pi = 0; pi < static_cast<int>(passes_.size()); ++pi) {
        Pass& p = passes_[pi];
        if (p.culled) continue;

        // Aliasing barriers: activate each placed transient before its first use.
        if (aliasingActive_) {
            std::vector<D3D12_RESOURCE_BARRIER> aliasBatch;
            for (uint32_t ti : transients_) {
                Resource& r = resources_[ti];
                if (r.firstUse != pi) continue;
                D3D12_RESOURCE_BARRIER ab{};
                ab.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                ab.Aliasing.pResourceBefore = (r.aliasPrev >= 0) ? resources_[r.aliasPrev].tex()->get() : nullptr;
                ab.Aliasing.pResourceAfter = r.tex()->get();
                aliasBatch.push_back(ab);
            }
            if (!aliasBatch.empty()) cmd->ResourceBarrier(static_cast<UINT>(aliasBatch.size()), aliasBatch.data());
        }

        std::vector<D3D12_RESOURCE_BARRIER> batch;
        for (const auto& a : p.accesses)
            transition(cmd, resources_[a.resource], stateFor(a.access), batch);
        if (!batch.empty()) cmd->ResourceBarrier(static_cast<UINT>(batch.size()), batch.data());

        PassContext ctx;
        ctx.cmd = cmd;
        ctx.graph = this;

        if (p.type == PassType::Raster) {
            // Bind colour + depth targets declared as writes, clear-on-first-write.
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
            uint32_t rtvCount = 0;
            const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
            uint32_t w = 0, h = 0;

            for (const auto& a : p.accesses) {
                Resource& r = resources_[a.resource];
                if (a.access == RGAccess::RenderTarget && rtvCount < 8) {
                    if (!r.rtvCreated) {
                        if (r.rtvSlot) heaps_->recreateRtv(r.rtv, *r.tex());
                        else { r.rtv = heaps_->createRtv(*r.tex()); r.rtvSlot = true; }
                        r.rtvCreated = true;
                    }
                    rtvs[rtvCount++] = r.rtv;
                    w = r.desc.width; h = r.desc.height;
                } else if (a.access == RGAccess::DepthWrite || a.access == RGAccess::DepthRead) {
                    if (!r.dsvCreated) {
                        const DXGI_FORMAT dfmt = r.desc.dsvFormat != DXGI_FORMAT_UNKNOWN ? r.desc.dsvFormat : r.desc.format;
                        if (r.dsvSlot) heaps_->recreateDsv(r.dsv, *r.tex(), dfmt);
                        else { r.dsv = heaps_->createDsv(*r.tex(), dfmt); r.dsvSlot = true; }
                        r.dsvCreated = true;
                    }
                    dsvHandle = r.dsv; dsvPtr = &dsvHandle;
                    if (!w) { w = r.desc.width; h = r.desc.height; }
                }
            }

            if (w && h) {
                const D3D12_VIEWPORT vp{ 0, 0, float(w), float(h), 0.0f, 1.0f };
                const D3D12_RECT rect{ 0, 0, LONG(w), LONG(h) };
                cmd->RSSetViewports(1, &vp);
                cmd->RSSetScissorRects(1, &rect);
            }
            cmd->OMSetRenderTargets(rtvCount, rtvCount ? rtvs : nullptr, FALSE, dsvPtr);

            // Clears (once per resource per frame, on first write).
            for (const auto& a : p.accesses) {
                Resource& r = resources_[a.resource];
                if (!a.isWrite || r.cleared || !r.desc.clear) continue;
                if (a.access == RGAccess::RenderTarget) {
                    cmd->ClearRenderTargetView(r.rtv, r.desc.clearColor, 0, nullptr);
                    r.cleared = true;
                } else if (a.access == RGAccess::DepthWrite) {
                    cmd->ClearDepthStencilView(r.dsv, D3D12_CLEAR_FLAG_DEPTH, r.desc.clearDepth, 0, 0, nullptr);
                    r.cleared = true;
                }
            }
            ctx.width = w; ctx.height = h;
            ctx.hasDepth = (dsvPtr != nullptr);
            if (dsvPtr) ctx.depthDsv = dsvHandle;
        } else {
            cmd->SetComputeRootSignature(rootSig_);
        }

        if (p.exec) p.exec(ctx);
    }
}

Texture* RenderGraph::findTexture(std::string_view name) {
    const int i = findResource(name);
    return i >= 0 ? resources_[i].tex() : nullptr;
}

D3D12_RESOURCE_STATES RenderGraph::stateOf(std::string_view name) {
    const int i = findResource(name);
    return i >= 0 ? resources_[i].state : D3D12_RESOURCE_STATE_COMMON;
}

} // namespace pulse::render
