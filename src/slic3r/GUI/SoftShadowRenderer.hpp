#pragma once

#include "GLModel.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace Slic3r {

class GLVolume;
class GLVolumeCollection;

namespace GUI {

struct Camera;
class PartPlateList;

/** @brief Per-frame inputs of the soft shadow update. */
struct SoftShadowFrameInput
{
    GLVolumeCollection* volumes{ nullptr };
    bool interactive{ false };
    bool renderSlaAuxiliaries{ true };
};

/** @brief Non-blocking GPU timer built on a ring of GL_TIME_ELAPSED queries. */
class SoftShadowGpuTimer
{
public:
    /** @brief Starts timing when a free query slot is available. */
    void Begin();

    /** @brief Ends the timing started by Begin(). */
    void End();

    /** @brief Releases all query objects; the GL context must be current. */
    void Release();

    /** @brief Returns the smoothed GPU time in milliseconds. */
    float GetTimeMs() const { return _timeMs; }

private:
    void Poll();

    std::array<unsigned int, 4> _queries{ { 0, 0, 0, 0 } };
    std::array<bool, 4> _pending{ { false, false, false, false } };
    unsigned int _next{ 0 };
    int _activeSlot{ -1 };
    float _timeMs{ 0.0f };
};

/**
 * @brief Bakes PCSS soft shadows of objects into a build-plate mask and composites it.
 * The mask is rebuilt only when the caster set changes, so idle frames cost one texture fetch per pixel.
 */
class SoftShadowRenderer
{
public:
    SoftShadowRenderer() = default;
    SoftShadowRenderer(const SoftShadowRenderer&) = delete;
    SoftShadowRenderer& operator=(const SoftShadowRenderer&) = delete;

    /** @brief Returns true when GL 3.3, ARB framebuffers and both shadow shaders are available. */
    static bool IsSupported();

    /**
     * @brief Rebuilds the shadow map and ground mask when the scene signature changed.
     * @param input Casters and interaction state; input.volumes must not be null.
     * @return true when a valid mask is available for RenderReceivers().
     */
    bool Update(const SoftShadowFrameInput& input);

    /**
     * @brief Composites the cached mask over all plates.
     * @param camera Camera of the current frame.
     * @param plates Plates used as shadow receivers.
     * @param darkMode Selects the shadow strength for the dark theme.
     */
    void RenderReceivers(const Camera& camera, PartPlateList& plates, bool darkMode);

    /** @brief Releases all GL objects; the GL context must be current. */
    void Release();

    /** @brief Returns true when GL resources are allocated. */
    bool HasResources() const { return _shadowFramebuffer != 0 || _maskFramebuffer != 0; }

    /** @brief Returns the smoothed GPU time of the last mask rebuild in milliseconds. */
    float GetUpdateGpuTimeMs() const { return _updateTimer.GetTimeMs(); }

    /** @brief Returns the smoothed GPU time of the receiver pass in milliseconds. */
    float GetReceiverGpuTimeMs() const { return _receiverTimer.GetTimeMs(); }

private:
    bool EnsureResources();
    bool CreateShadowMapResources();
    bool CreateMaskResources();
    bool CollectCasters(const SoftShadowFrameInput& input);
    std::size_t ComputeSignature(bool interactive) const;
    void FitLightFrustum();
    bool RenderShadowMap();
    void BuildFootprintModel();
    bool RenderGroundMask(bool interactive);

    unsigned int _shadowFramebuffer{ 0 };
    unsigned int _shadowDepthTexture{ 0 };
    unsigned int _maskFramebuffer{ 0 };
    unsigned int _maskTexture{ 0 };
    unsigned int _rawDepthSampler{ 0 };
    unsigned int _compareDepthSampler{ 0 };
    unsigned int _shadowMapSize{ 0 };
    unsigned int _maskSize{ 0 };
    bool _resourcesFailed{ false };

    std::vector<GLVolume*> _casters;
    std::vector<BoundingBoxf3> _instanceBoxes;
    BoundingBoxf3 _casterBox;
    GLModel _footprintModel;

    Transform3d _lightView{ Transform3d::Identity() };
    Matrix4d _lightProjection{ Matrix4d::Identity() };
    std::array<float, 4> _maskRect{ { 0.0f, 0.0f, 1.0f, 1.0f } };
    Vec2f _lightExtent{ Vec2f::Ones() };
    float _depthSpan{ 1.0f };
    float _maskUvScale{ 1.0f };

    std::size_t _lastSignature{ 0 };
    bool _lastWasInteractive{ false };
    bool _maskValid{ false };

    SoftShadowGpuTimer _updateTimer;
    SoftShadowGpuTimer _receiverTimer;
};

} // namespace GUI
} // namespace Slic3r
