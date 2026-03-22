//
// ImGui Metal C++ bridge — provides MTL::Device*/MTL::CommandBuffer* overloads
// that delegate to the Obj-C implementations compiled into the vcpkg imgui library.
//
// Needed because vcpkg builds imgui_impl_metal.mm without IMGUI_IMPL_METAL_CPP,
// so the C++ wrapper symbols are missing from the library.  This file provides
// the exact same wrapper functions that imgui_impl_metal.mm would generate if
// IMGUI_IMPL_METAL_CPP were defined during the vcpkg build.
//
//

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <imgui.h>
#include <imgui_impl_metal.h>    // Obj-C declarations (id<MTLDevice>, etc.)

// metal-cpp types for the C++ overloads
#include <Metal/Metal.hpp>
#include <QuartzCore/CAMetalDrawable.hpp>

// ── C++ overloads (match imgui_impl_metal.h IMGUI_IMPL_METAL_CPP section) ──

bool ImGui_ImplMetal_Init(MTL::Device* device)
{
    return ImGui_ImplMetal_Init((__bridge id<MTLDevice>)(device));
}

void ImGui_ImplMetal_NewFrame(MTL::RenderPassDescriptor* renderPassDescriptor)
{
    ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor*)(renderPassDescriptor));
}

void ImGui_ImplMetal_RenderDrawData(ImDrawData* draw_data,
                                    MTL::CommandBuffer* commandBuffer,
                                    MTL::RenderCommandEncoder* commandEncoder)
{
    ImGui_ImplMetal_RenderDrawData(draw_data,
                                  (__bridge id<MTLCommandBuffer>)(commandBuffer),
                                  (__bridge id<MTLRenderCommandEncoder>)(commandEncoder));
}

// Note: ImGui_ImplMetal_CreateFontsTexture() was removed in newer imgui versions.
// Font texture creation is now handled automatically via ImGui_ImplMetal_UpdateTexture().
// ImGui_ImplMetal_DestroyDeviceObjects() has identical signatures in both Obj-C and C++
// (void → void), so it doesn't need a bridge wrapper.

bool ImGui_ImplMetal_CreateDeviceObjects(MTL::Device* device)
{
    return ImGui_ImplMetal_CreateDeviceObjects((__bridge id<MTLDevice>)(device));
}
