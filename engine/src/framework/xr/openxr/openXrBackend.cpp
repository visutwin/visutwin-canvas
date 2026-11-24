// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#include "openXrBackend.h"

#include <spdlog/spdlog.h>

#ifndef VISUTWIN_XR_OPENXR

namespace visutwin::canvas
{
    std::shared_ptr<XrBackend> createOpenXrBackend(const OpenXrOptions& /*options*/)
    {
        spdlog::warn("createOpenXrBackend: the engine was built without OpenXR (VISUTWIN_XR_OPENXR)");
        return nullptr;
    }
}

#else

#include <algorithm>
#include <array>
#include <cstring>

#include <openxr/openxr.h>

#include "openXrGraphics.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr int kHands = 2;
        constexpr const char* kHandPaths[kHands] = {"/user/hand/left", "/user/hand/right"};

        std::string resultText(const XrInstance instance, const XrResult result)
        {
            char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
            if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
                return buffer;
            }
            return "XrResult " + std::to_string(static_cast<int>(result));
        }

        XrPosef toXr(const XrPose& pose)
        {
            XrPosef out;
            out.position = {pose.position.getX(), pose.position.getY(), pose.position.getZ()};
            out.orientation = {pose.rotation.getX(), pose.rotation.getY(), pose.rotation.getZ(), pose.rotation.getW()};
            return out;
        }

        // OpenXR's spaces are right-handed with -Z forward and +Y up, as the engine's.
        XrPose fromXr(const XrPosef& pose)
        {
            return {Vector3(pose.position.x, pose.position.y, pose.position.z),
                Quaternion(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w)};
        }

        class OpenXrBackend final : public XrBackend
        {
        public:
            explicit OpenXrBackend(const OpenXrOptions& options) : _options(options) {}
            ~OpenXrBackend() override
            {
                end();
                if (_instance != XR_NULL_HANDLE) {
                    xrDestroyInstance(_instance);
                }
            }

            [[nodiscard]] const char* name() const override { return "OpenXR"; }

            bool isAvailable(const XrSessionType type) override
            {
                std::string error;
                if (!ensureSystem(nullptr, error)) {
                    return false;
                }
                XrEnvironmentBlendMode mode;
                return chooseBlendMode(type, mode);
            }

            void configureGraphicsDevice(GraphicsDeviceOptions& options) override
            {
#ifdef VISUTWIN_HAS_VULKAN
                // Only a Vulkan device is shaped by the runtime. The instance is made with the
                // Vulkan binding's extension here and kept for the session.
                if (options.backend != Backend::Vulkan) {
                    return;
                }
                std::string error;
                if (!ensureSystem(openXrVulkanExtensionName(), error)) {
                    spdlog::info("OpenXR: the graphics device is created without the runtime's requirements ({})", error);
                    return;
                }
                configureOpenXrVulkanDevice(_instance, _systemId, options);
#else
                (void)options;
#endif
            }

            bool begin(const XrSessionOptions& options, GraphicsDevice& device, std::string& error) override
            {
                _graphics.reset();
#ifdef VISUTWIN_HAS_METAL
                _graphics = createOpenXrMetalGraphics(device);
#endif
#ifdef VISUTWIN_HAS_VULKAN
                if (!_graphics) {
                    _graphics = createOpenXrVulkanGraphics(device);
                }
#endif
                if (!_graphics) {
                    error = "the OpenXR backend has no graphics binding for this graphics device";
                    return false;
                }
                // The binding's extension has to be enabled on the instance: an instance made
                // without it (by isAvailable, before a device was known) is made again.
                if (!ensureSystem(_graphics->extensionName(), error)) {
                    return false;
                }
                _graphics->extensionsEnabled(_enabledExtensions);
                if (!chooseBlendMode(options.type, _blendMode)) {
                    error = "the OpenXR system has no blend mode for that session type";
                    return false;
                }
                if (!_graphics->checkRequirements(_instance, _systemId, error)) {
                    return false;
                }

                XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
                sessionInfo.next = _graphics->sessionBinding();
                sessionInfo.systemId = _systemId;
                if (const XrResult result = xrCreateSession(_instance, &sessionInfo, &_session); XR_FAILED(result)) {
                    _session = XR_NULL_HANDLE;
                    error = "xrCreateSession failed: " + resultText(_instance, result);
                    return false;
                }

                if (!createSpace(options.spaceType, error) || !createSwapchains(options, error) || !createActions(error)) {
                    end();
                    return false;
                }
                _running = true;
                _sessionBegun = false;
                _sessionState = XR_SESSION_STATE_IDLE;
                _frameWaited = false;
                _frameBegun = false;
                return true;
            }

            void end() override
            {
                if (_session == XR_NULL_HANDLE) {
                    _running = false;
                    return;
                }
                if (_frameBegun) {
                    endFrame(XrFrameSubmission{});
                }
                for (auto& swapchain : _swapchains) {
                    if (_graphics) {
                        _graphics->releaseImages(swapchain.images);
                    }
                    if (swapchain.handle != XR_NULL_HANDLE) {
                        xrDestroySwapchain(swapchain.handle);
                    }
                }
                _swapchains.clear();
                for (auto& hand : _hands) {
                    if (hand.gripSpace != XR_NULL_HANDLE) {
                        xrDestroySpace(hand.gripSpace);
                    }
                    if (hand.aimSpace != XR_NULL_HANDLE) {
                        xrDestroySpace(hand.aimSpace);
                    }
                    hand = {};
                }
                if (_actionSet != XR_NULL_HANDLE) {
                    xrDestroyActionSet(_actionSet);
                    _actionSet = XR_NULL_HANDLE;
                }
                if (_space != XR_NULL_HANDLE) {
                    xrDestroySpace(_space);
                    _space = XR_NULL_HANDLE;
                }
                // Destroying a session that is still running is allowed and ends it.
                xrDestroySession(_session);
                _session = XR_NULL_HANDLE;
                _sessionBegun = false;
                _running = false;
                _graphics.reset();
            }

            [[nodiscard]] bool running() const override { return _running; }

            [[nodiscard]] XrVisibility visibility() const override
            {
                switch (_sessionState) {
                case XR_SESSION_STATE_FOCUSED: return XrVisibility::Visible;
                case XR_SESSION_STATE_VISIBLE: return XrVisibility::VisibleBlurred;
                default: return XrVisibility::Hidden;
                }
            }

            [[nodiscard]] std::pair<int, int> viewSize() const override { return {_width, _height}; }

            bool waitFrame(XrFrameTiming& timing) override
            {
                pollEvents();
                timing.shouldRender = false;
                if (!_running) {
                    return false;
                }
                // Until the runtime is ready for the session there are no frames to wait for.
                if (!_sessionBegun) {
                    return true;
                }
                XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState frameState{XR_TYPE_FRAME_STATE};
                if (const XrResult result = xrWaitFrame(_session, &waitInfo, &frameState); XR_FAILED(result)) {
                    spdlog::warn("OpenXR: xrWaitFrame failed: {}", resultText(_instance, result));
                    return true;
                }
                _frameWaited = true;
                _displayTime = frameState.predictedDisplayTime;
                _shouldRender = frameState.shouldRender == XR_TRUE;
                timing.shouldRender = _shouldRender;
                timing.predictedDisplayTime = static_cast<double>(frameState.predictedDisplayTime) * 1e-9;
                timing.predictedDisplayPeriod = static_cast<double>(frameState.predictedDisplayPeriod) * 1e-9;
                return true;
            }

            bool locateViews(std::vector<XrViewState>& views) override
            {
                if (!_sessionBegun || !_frameWaited) {
                    return false;
                }
                XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
                locateInfo.viewConfigurationType = _viewConfiguration;
                locateInfo.displayTime = _displayTime;
                locateInfo.space = _space;
                ::XrViewState viewState{XR_TYPE_VIEW_STATE};
                std::vector<::XrView> located(_viewCount, {XR_TYPE_VIEW});
                uint32_t count = 0;
                if (XR_FAILED(xrLocateViews(_session, &locateInfo, &viewState, _viewCount, &count, located.data()))) {
                    return false;
                }
                constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
                if ((viewState.viewStateFlags & kValid) != kValid) {
                    return false;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    visutwin::canvas::XrViewState state;
                    state.eye = count == 1 ? XrEye::None : (i == 0 ? XrEye::Left : XrEye::Right);
                    state.pose = fromXr(located[i].pose);
                    state.fov = {located[i].fov.angleLeft, located[i].fov.angleRight, located[i].fov.angleUp,
                        located[i].fov.angleDown};
                    views.push_back(state);
                }
                return count > 0;
            }

            void pollInput(std::vector<XrInputSourceState>& sources) override
            {
                // Actions are live only while the session has focus.
                if (!_sessionBegun || !_frameWaited || _sessionState != XR_SESSION_STATE_FOCUSED ||
                    _actionSet == XR_NULL_HANDLE) {
                    return;
                }
                const XrActiveActionSet active{_actionSet, XR_NULL_PATH};
                XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
                syncInfo.countActiveActionSets = 1;
                syncInfo.activeActionSets = &active;
                if (XR_FAILED(xrSyncActions(_session, &syncInfo))) {
                    return;
                }
                for (int h = 0; h < kHands; ++h) {
                    Hand& hand = _hands[h];
                    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
                    getInfo.subactionPath = hand.path;

                    XrActionStatePose gripState{XR_TYPE_ACTION_STATE_POSE};
                    XrActionStatePose aimState{XR_TYPE_ACTION_STATE_POSE};
                    getInfo.action = _gripAction;
                    xrGetActionStatePose(_session, &getInfo, &gripState);
                    getInfo.action = _aimAction;
                    xrGetActionStatePose(_session, &getInfo, &aimState);
                    if (!gripState.isActive && !aimState.isActive) {
                        continue;
                    }

                    XrInputSourceState state;
                    state.id = static_cast<uint32_t>(h + 1);
                    state.handedness = h == 0 ? XrHandedness::Left : XrHandedness::Right;
                    state.targetRayMode = XrTargetRayMode::TrackedPointer;
                    state.profiles = hand.profiles;

                    constexpr XrSpaceLocationFlags kValid =
                        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
                    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                    location.next = &velocity;
                    if (gripState.isActive && XR_SUCCEEDED(xrLocateSpace(hand.gripSpace, _space, _displayTime, &location)) &&
                        (location.locationFlags & kValid) == kValid) {
                        state.gripValid = true;
                        state.grip = fromXr(location.pose);
                        if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
                            state.velocityValid = true;
                            state.linearVelocity = Vector3(velocity.linearVelocity.x, velocity.linearVelocity.y,
                                velocity.linearVelocity.z);
                        }
                    }
                    XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
                    if (aimState.isActive && XR_SUCCEEDED(xrLocateSpace(hand.aimSpace, _space, _displayTime, &aim)) &&
                        (aim.locationFlags & kValid) == kValid) {
                        state.rayValid = true;
                        state.ray = fromXr(aim.pose);
                    }

                    const auto readFloat = [&](const XrAction action) {
                        XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
                        getInfo.action = action;
                        return XR_SUCCEEDED(xrGetActionStateFloat(_session, &getInfo, &value)) && value.isActive
                            ? value.currentState : 0.0f;
                    };
                    const auto readVector = [&](const XrAction action) {
                        XrActionStateVector2f value{XR_TYPE_ACTION_STATE_VECTOR2F};
                        getInfo.action = action;
                        return XR_SUCCEEDED(xrGetActionStateVector2f(_session, &getInfo, &value)) && value.isActive
                            ? value.currentState : XrVector2f{0.0f, 0.0f};
                    };
                    state.selectValue = readFloat(_selectAction);
                    state.squeezeValue = readFloat(_squeezeAction);
                    // Hysteresis, so a trigger held near the middle does not chatter.
                    hand.selecting = state.selectValue > (hand.selecting ? 0.4f : 0.6f);
                    hand.squeezing = state.squeezeValue > (hand.squeezing ? 0.4f : 0.6f);
                    state.select = hand.selecting;
                    state.squeeze = hand.squeezing;
                    const XrVector2f trackpad = readVector(_trackpadAction);
                    const XrVector2f thumbstick = readVector(_thumbstickAction);
                    state.buttons = {state.selectValue, state.squeezeValue, readFloat(_trackpadClickAction),
                        readFloat(_thumbstickClickAction), readFloat(_primaryAction), readFloat(_secondaryAction)};
                    // OpenXR's y points away from the user; the gamepad layout's toward them.
                    state.axes = {trackpad.x, -trackpad.y, thumbstick.x, -thumbstick.y};
                    sources.push_back(std::move(state));
                }
            }

            bool beginFrame(std::vector<XrViewTarget>& targets) override
            {
                if (!_sessionBegun || !_frameWaited) {
                    return false;
                }
                _frameWaited = false;
                XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
                if (const XrResult result = xrBeginFrame(_session, &beginInfo); XR_FAILED(result)) {
                    spdlog::warn("OpenXR: xrBeginFrame failed: {}", resultText(_instance, result));
                    return false;
                }
                _frameBegun = true;
                _imagesAcquired = false;
                if (!_shouldRender) {
                    return false;
                }
                for (auto& swapchain : _swapchains) {
                    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    waitInfo.timeout = XR_INFINITE_DURATION;
                    uint32_t index = 0;
                    swapchain.acquired = XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &index)) &&
                        index < swapchain.images.size();
                    if (!swapchain.acquired) {
                        releaseAcquired();
                        targets.clear();
                        return false;
                    }
                    swapchain.current = index;
                    if (XR_FAILED(xrWaitSwapchainImage(swapchain.handle, &waitInfo))) {
                        releaseAcquired();
                        targets.clear();
                        return false;
                    }
                    _graphics->imageAcquired(swapchain.images[index]);
                    targets.push_back({swapchain.images[index].target,
                        Vector4(0.0f, 0.0f, static_cast<float>(_width), static_cast<float>(_height))});
                }
                _imagesAcquired = true;
                return true;
            }

            void finishFrame() override
            {
                if (!_frameBegun || !_graphics) {
                    return;
                }
                for (auto& swapchain : _swapchains) {
                    if (swapchain.acquired && swapchain.current < swapchain.images.size()) {
                        _graphics->prepareRelease(swapchain.images[swapchain.current]);
                    }
                }
            }

            void endFrame(const XrFrameSubmission& submission) override
            {
                if (!_frameBegun) {
                    return;
                }
                _frameBegun = false;
                releaseAcquired();

                // One projection layer per depth slice, farthest first, each over the ones
                // before it by its alpha.
                const int slices = _slices;
                const bool draw = submission.rendered && _imagesAcquired &&
                    submission.views.size() == static_cast<size_t>(_viewCount) && submission.depthSlices == slices;
                std::vector<XrCompositionLayerProjectionView> projectionViews;
                std::vector<XrCompositionLayerProjection> layers;
                std::vector<const XrCompositionLayerBaseHeader*> layerPointers;
                if (draw) {
                    projectionViews.resize(static_cast<size_t>(slices) * _viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                    layers.resize(static_cast<size_t>(slices), {XR_TYPE_COMPOSITION_LAYER_PROJECTION});
                    for (int layer = 0; layer < slices; ++layer) {
                        const int slice = slices - 1 - layer;
                        for (uint32_t v = 0; v < _viewCount; ++v) {
                            auto& view = projectionViews[static_cast<size_t>(layer) * _viewCount + v];
                            const visutwin::canvas::XrViewState& state = submission.views[v];
                            view.pose = toXr(state.pose);
                            view.fov = {state.fov.left, state.fov.right, state.fov.up, state.fov.down};
                            view.subImage.swapchain = _swapchains[v * static_cast<size_t>(slices) + static_cast<size_t>(slice)].handle;
                            view.subImage.imageRect = {{0, 0}, {_width, _height}};
                            view.subImage.imageArrayIndex = 0;
                        }
                        XrCompositionLayerProjection& projection = layers[static_cast<size_t>(layer)];
                        projection.space = _space;
                        projection.viewCount = _viewCount;
                        projection.views = &projectionViews[static_cast<size_t>(layer) * _viewCount];
                        // The engine's colour is not premultiplied. The farthest layer of an
                        // opaque session covers everything; every other layer blends.
                        const bool blends = layer > 0 || _blendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                        projection.layerFlags = blends
                            ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
                            : 0;
                        layerPointers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
                    }
                }
                XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
                endInfo.displayTime = _displayTime;
                endInfo.environmentBlendMode = _blendMode;
                endInfo.layerCount = static_cast<uint32_t>(layerPointers.size());
                endInfo.layers = layerPointers.empty() ? nullptr : layerPointers.data();
                if (const XrResult result = xrEndFrame(_session, &endInfo); XR_FAILED(result)) {
                    spdlog::warn("OpenXR: xrEndFrame failed: {}", resultText(_instance, result));
                }
            }

            [[nodiscard]] Texture* viewTexture(const int view, const int slice) const override
            {
                const size_t index = static_cast<size_t>(view) * static_cast<size_t>(_slices) + static_cast<size_t>(slice);
                if (view < 0 || slice < 0 || slice >= _slices || index >= _swapchains.size()) {
                    return nullptr;
                }
                const Swapchain& swapchain = _swapchains[index];
                return swapchain.current < swapchain.images.size() ? swapchain.images[swapchain.current].texture.get() : nullptr;
            }

            void pulse(const uint32_t inputSourceId, const float intensity, const float seconds) override
            {
                if (!_sessionBegun || inputSourceId < 1 || inputSourceId > kHands || _hapticAction == XR_NULL_HANDLE) {
                    return;
                }
                XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                vibration.amplitude = std::clamp(intensity, 0.0f, 1.0f);
                vibration.duration = static_cast<XrDuration>(static_cast<double>(seconds) * 1e9);
                vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
                XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
                info.action = _hapticAction;
                info.subactionPath = _hands[inputSourceId - 1].path;
                xrApplyHapticFeedback(_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
            }

        private:
            struct Swapchain
            {
                XrSwapchain handle = XR_NULL_HANDLE;
                std::vector<OpenXrSwapchainImage> images;
                uint32_t current = 0;
                bool acquired = false;
            };

            struct Hand
            {
                XrPath path = XR_NULL_PATH;
                XrSpace gripSpace = XR_NULL_HANDLE;
                XrSpace aimSpace = XR_NULL_HANDLE;
                std::vector<std::string> profiles;
                bool selecting = false;
                bool squeezing = false;
            };

            /// The instance (with the graphics binding's extension when one is given) and the
            /// system. An instance made without the extension a binding now needs is made again.
            bool ensureSystem(const char* extension, std::string& error)
            {
                if (_instance != XR_NULL_HANDLE && extension && _instanceExtension != extension) {
                    xrDestroyInstance(_instance);
                    _instance = XR_NULL_HANDLE;
                }
                if (_instance == XR_NULL_HANDLE) {
                    uint32_t count = 0;
                    // Fails when no runtime is installed or active: the ordinary case on a
                    // machine without a headset.
                    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) {
                        error = "no OpenXR runtime is available";
                        return false;
                    }
                    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
                    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
                    if (extension && std::ranges::none_of(extensions, [&](const auto& candidate) {
                            return std::strcmp(candidate.extensionName, extension) == 0; })) {
                        error = std::string("the OpenXR runtime does not support ") + extension;
                        return false;
                    }

                    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
                    std::strncpy(createInfo.applicationInfo.applicationName, _options.applicationName.c_str(),
                        XR_MAX_APPLICATION_NAME_SIZE - 1);
                    std::strncpy(createInfo.applicationInfo.engineName, "VisuTwin Engine", XR_MAX_ENGINE_NAME_SIZE - 1);
                    createInfo.applicationInfo.applicationVersion = 1;
                    createInfo.applicationInfo.engineVersion = 1;
                    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
                    std::vector<const char*> enabled;
                    if (extension) {
                        enabled.push_back(extension);
                    }
                    for (const char* optional : kOpenXrOptionalExtensions) {
                        if (std::ranges::any_of(extensions, [&](const auto& candidate) {
                                return std::strcmp(candidate.extensionName, optional) == 0; })) {
                            enabled.push_back(optional);
                        }
                    }
                    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
                    createInfo.enabledExtensionNames = enabled.empty() ? nullptr : enabled.data();
                    _enabledExtensions.assign(enabled.begin(), enabled.end());
                    if (const XrResult result = xrCreateInstance(&createInfo, &_instance); XR_FAILED(result)) {
                        _instance = XR_NULL_HANDLE;
                        error = "xrCreateInstance failed: " + resultText(XR_NULL_HANDLE, result);
                        return false;
                    }
                    _instanceExtension = extension ? extension : "";
                    _systemId = XR_NULL_SYSTEM_ID;
                }

                if (_systemId == XR_NULL_SYSTEM_ID) {
                    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
                    systemInfo.formFactor = _options.handheld ? XR_FORM_FACTOR_HANDHELD_DISPLAY
                        : XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                    // Fails while the headset is not connected; asked again next time.
                    if (const XrResult result = xrGetSystem(_instance, &systemInfo, &_systemId); XR_FAILED(result)) {
                        _systemId = XR_NULL_SYSTEM_ID;
                        error = "no OpenXR system: " + resultText(_instance, result);
                        return false;
                    }
                    _viewConfiguration = _options.handheld ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO
                        : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                }
                return true;
            }

            bool chooseBlendMode(const XrSessionType type, XrEnvironmentBlendMode& mode) const
            {
                uint32_t count = 0;
                if (XR_FAILED(xrEnumerateEnvironmentBlendModes(_instance, _systemId, _viewConfiguration, 0, &count, nullptr)) ||
                    count == 0) {
                    return false;
                }
                std::vector<XrEnvironmentBlendMode> modes(count);
                xrEnumerateEnvironmentBlendModes(_instance, _systemId, _viewConfiguration, count, &count, modes.data());
                const auto has = [&](const XrEnvironmentBlendMode wanted) {
                    return std::ranges::find(modes, wanted) != modes.end();
                };
                if (type == XrSessionType::Vr) {
                    // A see-through display has no opaque mode; a virtual world is still drawn on it.
                    mode = has(XR_ENVIRONMENT_BLEND_MODE_OPAQUE) ? XR_ENVIRONMENT_BLEND_MODE_OPAQUE : modes.front();
                    return true;
                }
                if (has(XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)) {
                    mode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                    return true;
                }
                if (has(XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)) {
                    mode = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
                    return true;
                }
                return false;
            }

            bool createSpace(const XrSpaceType type, std::string& error)
            {
                uint32_t count = 0;
                xrEnumerateReferenceSpaces(_session, 0, &count, nullptr);
                std::vector<XrReferenceSpaceType> offered(count);
                xrEnumerateReferenceSpaces(_session, count, &count, offered.data());
                const auto has = [&](const XrReferenceSpaceType wanted) {
                    return std::ranges::find(offered, wanted) != offered.end();
                };
                XrReferenceSpaceType chosen = XR_REFERENCE_SPACE_TYPE_LOCAL;
                switch (type) {
                case XrSpaceType::Viewer:
                    chosen = XR_REFERENCE_SPACE_TYPE_VIEW;
                    break;
                case XrSpaceType::LocalFloor:
                    // The floor-level local space where the runtime has it, else the stage,
                    // whose origin is on the floor too.
                    chosen = has(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR
                        : has(XR_REFERENCE_SPACE_TYPE_STAGE) ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
                    break;
                case XrSpaceType::BoundedFloor:
                    chosen = has(XR_REFERENCE_SPACE_TYPE_STAGE) ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
                    break;
                case XrSpaceType::Local:
                case XrSpaceType::Unbounded:
                    break;
                }
                XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                spaceInfo.referenceSpaceType = chosen;
                spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
                if (const XrResult result = xrCreateReferenceSpace(_session, &spaceInfo, &_space); XR_FAILED(result)) {
                    _space = XR_NULL_HANDLE;
                    error = "xrCreateReferenceSpace failed: " + resultText(_instance, result);
                    return false;
                }
                return true;
            }

            bool createSwapchains(const XrSessionOptions& options, std::string& error)
            {
                uint32_t viewCount = 0;
                xrEnumerateViewConfigurationViews(_instance, _systemId, _viewConfiguration, 0, &viewCount, nullptr);
                std::vector<XrViewConfigurationView> configurationViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
                if (viewCount == 0 || XR_FAILED(xrEnumerateViewConfigurationViews(_instance, _systemId, _viewConfiguration,
                        viewCount, &viewCount, configurationViews.data()))) {
                    error = "the OpenXR system reports no views";
                    return false;
                }
                _viewCount = viewCount;
                const float scale = std::max(options.framebufferScaleFactor, 0.1f);
                _width = std::clamp(static_cast<int32_t>(static_cast<float>(configurationViews[0].recommendedImageRectWidth) * scale),
                    1, static_cast<int32_t>(configurationViews[0].maxImageRectWidth));
                _height = std::clamp(static_cast<int32_t>(static_cast<float>(configurationViews[0].recommendedImageRectHeight) * scale),
                    1, static_cast<int32_t>(configurationViews[0].maxImageRectHeight));
                _slices = std::max(options.depthSlices, 1);

                uint32_t formatCount = 0;
                xrEnumerateSwapchainFormats(_session, 0, &formatCount, nullptr);
                std::vector<int64_t> formats(formatCount);
                xrEnumerateSwapchainFormats(_session, formatCount, &formatCount, formats.data());
                const int64_t format = _graphics->chooseColorFormat(formats);
                if (format == 0) {
                    error = "the OpenXR runtime offers no 8-bit RGBA swapchain format";
                    return false;
                }

                for (uint32_t view = 0; view < viewCount; ++view) {
                    for (int slice = 0; slice < _slices; ++slice) {
                        XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                        // Sampled by the window mirror, and a transfer source so the engine's
                        // own copies and reads of a view work on it.
                        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                            XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
                        createInfo.next = _graphics->swapchainCreateNext(format);
                        createInfo.format = format;
                        // The engine multisamples in a buffer of its own and resolves into the image.
                        createInfo.sampleCount = 1;
                        createInfo.width = static_cast<uint32_t>(_width);
                        createInfo.height = static_cast<uint32_t>(_height);
                        createInfo.faceCount = 1;
                        createInfo.arraySize = 1;
                        createInfo.mipCount = 1;
                        Swapchain swapchain;
                        if (const XrResult result = xrCreateSwapchain(_session, &createInfo, &swapchain.handle); XR_FAILED(result)) {
                            error = "xrCreateSwapchain failed: " + resultText(_instance, result);
                            return false;
                        }
                        _swapchains.push_back(std::move(swapchain));
                        const std::string name = "OpenXrView" + std::to_string(view) + "Slice" + std::to_string(slice);
                        if (!_graphics->wrapImages(_swapchains.back().handle, format, _width, _height, options.samples, name,
                                _swapchains.back().images, error)) {
                            return false;
                        }
                    }
                }
                return true;
            }

            XrAction createAction(const char* name, const XrActionType type, const XrPath* subactionPaths)
            {
                XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
                info.actionType = type;
                std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
                std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
                info.countSubactionPaths = kHands;
                info.subactionPaths = subactionPaths;
                XrAction action = XR_NULL_HANDLE;
                if (XR_FAILED(xrCreateAction(_actionSet, &info, &action))) {
                    return XR_NULL_HANDLE;
                }
                return action;
            }

            /// Suggests the bindings of one interaction profile: pairs of action and the input's
            /// path under a hand ("/input/trigger/value"), bound for both hands. A profile the
            /// runtime does not know is passed over.
            void suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& inputs)
            {
                XrPath profilePath = XR_NULL_PATH;
                if (XR_FAILED(xrStringToPath(_instance, profile, &profilePath))) {
                    return;
                }
                std::vector<XrActionSuggestedBinding> bindings;
                for (const auto& [action, input] : inputs) {
                    if (action == XR_NULL_HANDLE) {
                        continue;
                    }
                    for (const char* hand : kHandPaths) {
                        XrPath path = XR_NULL_PATH;
                        if (XR_SUCCEEDED(xrStringToPath(_instance, (std::string(hand) + input).c_str(), &path))) {
                            bindings.push_back({action, path});
                        }
                    }
                }
                XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                suggested.interactionProfile = profilePath;
                suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
                suggested.suggestedBindings = bindings.data();
                if (const XrResult result = xrSuggestInteractionProfileBindings(_instance, &suggested); XR_FAILED(result)) {
                    spdlog::debug("OpenXR: bindings for {} were not accepted: {}", profile, resultText(_instance, result));
                }
            }

            bool createActions(std::string& error)
            {
                XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
                std::strncpy(setInfo.actionSetName, "visutwin", XR_MAX_ACTION_SET_NAME_SIZE - 1);
                std::strncpy(setInfo.localizedActionSetName, "VisuTwin", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
                if (const XrResult result = xrCreateActionSet(_instance, &setInfo, &_actionSet); XR_FAILED(result)) {
                    _actionSet = XR_NULL_HANDLE;
                    error = "xrCreateActionSet failed: " + resultText(_instance, result);
                    return false;
                }
                XrPath handPaths[kHands];
                for (int h = 0; h < kHands; ++h) {
                    xrStringToPath(_instance, kHandPaths[h], &handPaths[h]);
                    _hands[h].path = handPaths[h];
                }
                _gripAction = createAction("grip_pose", XR_ACTION_TYPE_POSE_INPUT, handPaths);
                _aimAction = createAction("aim_pose", XR_ACTION_TYPE_POSE_INPUT, handPaths);
                _selectAction = createAction("select", XR_ACTION_TYPE_FLOAT_INPUT, handPaths);
                _squeezeAction = createAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, handPaths);
                _thumbstickAction = createAction("thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, handPaths);
                _thumbstickClickAction = createAction("thumbstick_click", XR_ACTION_TYPE_FLOAT_INPUT, handPaths);
                _trackpadAction = createAction("trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT, handPaths);
                _trackpadClickAction = createAction("trackpad_click", XR_ACTION_TYPE_FLOAT_INPUT, handPaths);
                _primaryAction = createAction("primary_button", XR_ACTION_TYPE_FLOAT_INPUT, handPaths);
                _secondaryAction = createAction("secondary_button", XR_ACTION_TYPE_FLOAT_INPUT, handPaths);
                _hapticAction = createAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, handPaths);
                if (_gripAction == XR_NULL_HANDLE || _aimAction == XR_NULL_HANDLE || _selectAction == XR_NULL_HANDLE) {
                    error = "the OpenXR input actions could not be created";
                    return false;
                }

                // A float action bound to a boolean input reads 0 or 1. The two face buttons
                // have different names on each hand, so they are suggested per hand below.
                suggest("/interaction_profiles/khr/simple_controller", {
                    {_gripAction, "/input/grip/pose"}, {_aimAction, "/input/aim/pose"},
                    {_selectAction, "/input/select/click"}, {_primaryAction, "/input/menu/click"},
                    {_hapticAction, "/output/haptic"}});
                suggest("/interaction_profiles/htc/vive_controller", {
                    {_gripAction, "/input/grip/pose"}, {_aimAction, "/input/aim/pose"},
                    {_selectAction, "/input/trigger/value"}, {_squeezeAction, "/input/squeeze/click"},
                    {_trackpadAction, "/input/trackpad"}, {_trackpadClickAction, "/input/trackpad/click"},
                    {_primaryAction, "/input/menu/click"}, {_hapticAction, "/output/haptic"}});
                suggest("/interaction_profiles/valve/index_controller", {
                    {_gripAction, "/input/grip/pose"}, {_aimAction, "/input/aim/pose"},
                    {_selectAction, "/input/trigger/value"}, {_squeezeAction, "/input/squeeze/value"},
                    {_thumbstickAction, "/input/thumbstick"}, {_thumbstickClickAction, "/input/thumbstick/click"},
                    {_trackpadAction, "/input/trackpad"}, {_primaryAction, "/input/a/click"},
                    {_secondaryAction, "/input/b/click"}, {_hapticAction, "/output/haptic"}});
                suggest("/interaction_profiles/microsoft/motion_controller", {
                    {_gripAction, "/input/grip/pose"}, {_aimAction, "/input/aim/pose"},
                    {_selectAction, "/input/trigger/value"}, {_squeezeAction, "/input/squeeze/click"},
                    {_thumbstickAction, "/input/thumbstick"}, {_thumbstickClickAction, "/input/thumbstick/click"},
                    {_trackpadAction, "/input/trackpad"}, {_trackpadClickAction, "/input/trackpad/click"},
                    {_primaryAction, "/input/menu/click"}, {_hapticAction, "/output/haptic"}});
                suggestTouch();

                for (int h = 0; h < kHands; ++h) {
                    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                    spaceInfo.subactionPath = handPaths[h];
                    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
                    spaceInfo.action = _gripAction;
                    xrCreateActionSpace(_session, &spaceInfo, &_hands[h].gripSpace);
                    spaceInfo.action = _aimAction;
                    xrCreateActionSpace(_session, &spaceInfo, &_hands[h].aimSpace);
                }

                XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
                attachInfo.countActionSets = 1;
                attachInfo.actionSets = &_actionSet;
                if (const XrResult result = xrAttachSessionActionSets(_session, &attachInfo); XR_FAILED(result)) {
                    error = "xrAttachSessionActionSets failed: " + resultText(_instance, result);
                    return false;
                }
                return true;
            }

            /// The Touch controller names its face buttons per hand (x and y on the left, a and
            /// b on the right), so its bindings are written out hand by hand.
            void suggestTouch()
            {
                XrPath profilePath = XR_NULL_PATH;
                if (XR_FAILED(xrStringToPath(_instance, "/interaction_profiles/oculus/touch_controller", &profilePath))) {
                    return;
                }
                std::vector<XrActionSuggestedBinding> bindings;
                const auto bind = [&](const XrAction action, const std::string& path) {
                    XrPath xrPath = XR_NULL_PATH;
                    if (action != XR_NULL_HANDLE && XR_SUCCEEDED(xrStringToPath(_instance, path.c_str(), &xrPath))) {
                        bindings.push_back({action, xrPath});
                    }
                };
                for (int h = 0; h < kHands; ++h) {
                    const std::string hand = kHandPaths[h];
                    bind(_gripAction, hand + "/input/grip/pose");
                    bind(_aimAction, hand + "/input/aim/pose");
                    bind(_selectAction, hand + "/input/trigger/value");
                    bind(_squeezeAction, hand + "/input/squeeze/value");
                    bind(_thumbstickAction, hand + "/input/thumbstick");
                    bind(_thumbstickClickAction, hand + "/input/thumbstick/click");
                    bind(_primaryAction, hand + (h == 0 ? "/input/x/click" : "/input/a/click"));
                    bind(_secondaryAction, hand + (h == 0 ? "/input/y/click" : "/input/b/click"));
                    bind(_hapticAction, hand + "/output/haptic");
                }
                XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                suggested.interactionProfile = profilePath;
                suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
                suggested.suggestedBindings = bindings.data();
                xrSuggestInteractionProfileBindings(_instance, &suggested);
            }

            /// The profile names each hand reports, most specific first, as upstream's input
            /// sources carry them.
            void updateProfiles()
            {
                for (auto& hand : _hands) {
                    hand.profiles.clear();
                    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
                    if (XR_FAILED(xrGetCurrentInteractionProfile(_session, hand.path, &state)) ||
                        state.interactionProfile == XR_NULL_PATH) {
                        continue;
                    }
                    char buffer[XR_MAX_PATH_LENGTH] = {};
                    uint32_t length = 0;
                    if (XR_SUCCEEDED(xrPathToString(_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &length, buffer))) {
                        // "/interaction_profiles/oculus/touch_controller" -> "oculus-touch-controller"
                        std::string name = buffer;
                        constexpr std::string_view prefix = "/interaction_profiles/";
                        if (name.starts_with(prefix)) {
                            name.erase(0, prefix.size());
                        }
                        std::ranges::replace(name, '/', '-');
                        std::ranges::replace(name, '_', '-');
                        hand.profiles.push_back(std::move(name));
                    }
                    hand.profiles.emplace_back("generic-trigger-squeeze");
                }
            }

            void pollEvents()
            {
                if (_instance == XR_NULL_HANDLE) {
                    return;
                }
                XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                while (xrPollEvent(_instance, &event) == XR_SUCCESS) {
                    switch (event.type) {
                    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                        const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                        if (changed.session != _session) {
                            break;
                        }
                        _sessionState = changed.state;
                        if (changed.state == XR_SESSION_STATE_READY) {
                            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                            beginInfo.primaryViewConfigurationType = _viewConfiguration;
                            _sessionBegun = XR_SUCCEEDED(xrBeginSession(_session, &beginInfo));
                        } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                            xrEndSession(_session);
                            _sessionBegun = false;
                            _frameWaited = false;
                        } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                            _running = false;
                        }
                        break;
                    }
                    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                        _running = false;
                        break;
                    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                        updateProfiles();
                        break;
                    default:
                        break;
                    }
                    event = {XR_TYPE_EVENT_DATA_BUFFER};
                }
            }

            void releaseAcquired()
            {
                for (auto& swapchain : _swapchains) {
                    if (swapchain.acquired) {
                        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);
                        swapchain.acquired = false;
                    }
                }
            }

            OpenXrOptions _options;
            std::unique_ptr<OpenXrGraphics> _graphics;

            XrInstance _instance = XR_NULL_HANDLE;
            std::string _instanceExtension;
            std::vector<std::string> _enabledExtensions;
            XrSystemId _systemId = XR_NULL_SYSTEM_ID;
            XrViewConfigurationType _viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XrEnvironmentBlendMode _blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            XrSession _session = XR_NULL_HANDLE;
            XrSessionState _sessionState = XR_SESSION_STATE_UNKNOWN;
            XrSpace _space = XR_NULL_HANDLE;
            bool _running = false;
            bool _sessionBegun = false;

            uint32_t _viewCount = 0;
            int32_t _width = 0;
            int32_t _height = 0;
            int _slices = 1;
            std::vector<Swapchain> _swapchains;

            bool _frameWaited = false;
            bool _frameBegun = false;
            bool _shouldRender = false;
            bool _imagesAcquired = false;
            XrTime _displayTime = 0;

            XrActionSet _actionSet = XR_NULL_HANDLE;
            XrAction _gripAction = XR_NULL_HANDLE;
            XrAction _aimAction = XR_NULL_HANDLE;
            XrAction _selectAction = XR_NULL_HANDLE;
            XrAction _squeezeAction = XR_NULL_HANDLE;
            XrAction _thumbstickAction = XR_NULL_HANDLE;
            XrAction _thumbstickClickAction = XR_NULL_HANDLE;
            XrAction _trackpadAction = XR_NULL_HANDLE;
            XrAction _trackpadClickAction = XR_NULL_HANDLE;
            XrAction _primaryAction = XR_NULL_HANDLE;
            XrAction _secondaryAction = XR_NULL_HANDLE;
            XrAction _hapticAction = XR_NULL_HANDLE;
            std::array<Hand, kHands> _hands;
        };
    }

    std::shared_ptr<XrBackend> createOpenXrBackend(const OpenXrOptions& options)
    {
        return std::make_shared<OpenXrBackend>(options);
    }
}

#endif
