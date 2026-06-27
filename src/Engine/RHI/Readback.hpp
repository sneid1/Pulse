#pragma once

#include "Engine/RHI/Device.hpp"
#include "Engine/RHI/Resource.hpp"
#include "Engine/Core/Image.hpp"

namespace pulse::rhi {

// Read an R8G8B8A8_UNORM texture back to a CPU Image. Records a transition +
// CopyTextureRegion to a readback buffer on the graphics queue, flushes, then
// de-pads the 256-byte-aligned rows. This is the single readback the plan allows
//, on capture only, never per frame. `currentState` is the texture's state on
// entry; it is restored afterwards.
Image readbackTexture(Device& device, const Texture& tex, D3D12_RESOURCE_STATES currentState);

} // namespace pulse::rhi
