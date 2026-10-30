#pragma once

#include "libslic3r/Point.hpp"

#include <array>
#include <string>

namespace Slic3r {
namespace GUI {

/// @brief Prefiltered IBL environment: GGX cube map mips + diffuse SH, baked offline by
///        scripts/bake_pbr_environment.py into resources/materials/pbr_environment.bin.
class PbrEnvironment
{
public:
    // Texture units of the PBR samplers, unit 0 is left to 2D textures.
    static const int ENV_TEXTURE_UNIT = 1;
    static const int NOISE_TEXTURE_UNIT = 2;

    /// @brief Get the shared environment. Main thread only.
    /// @return the environment instance.
    static PbrEnvironment& Instance();

    /// @brief Rotation from eye space to environment space. The environment is fixed in the world:
    ///        its up axis is world +Z (bed normal) and the panorama center faces the printer front (world -Y).
    /// @param viewMatrix camera view matrix (rotation + translation).
    /// @return eye space to environment space rotation.
    static Matrix3f ViewToEnvironment(const Transform3d& viewMatrix);

    /// @brief Load the resource and create the GL cube map once, needs a current GL 3.1+ context.
    /// @return true if the cube map is ready to bind.
    bool EnsureLoaded();

    /// @brief Get the GL cube map texture.
    /// @return texture id, 0 if not loaded.
    unsigned int GetTextureId() const { return _textureId; }

    /// @brief Get the last mip level, mapped to roughness 1.
    /// @return the max lod.
    float GetMaxLod() const { return _maxLod; }

    /// @brief Get diffuse SH coefficients, sum(sh[i] * Y_i(n)) = irradiance(n) / pi.
    /// @return the 9 coefficients.
    const std::array<float, 9>& GetSh() const { return _sh; }

    /// @brief Generate the tileable 3D noise of the material surface pattern once, needs a current GL 3.1+ context.
    ///        Texel: r = value, gba = gradient, packed as described in gouraud.fs.
    /// @return true if the 3D texture is ready to bind.
    bool EnsureNoiseLoaded();

    /// @brief Get the GL 3D noise texture (repeat wrap, with mipmaps).
    /// @return texture id, 0 if not created.
    unsigned int GetNoiseTextureId() const { return _noiseId; }

private:
    PbrEnvironment() = default;

    /// @brief Read and validate the baked file, then upload all mips.
    /// @param path full path of the baked file.
    /// @return false if the file is missing or invalid.
    bool LoadAndUpload(const std::string& path);

    /// @brief Build the noise volume on the CPU and upload it with mipmaps.
    /// @return false if the GL texture can not be created.
    bool CreateNoiseTexture();

    // GL objects live until exit, the shared context is gone before static destructors run.
    unsigned int _textureId{ 0 };
    float _maxLod{ 0.0f };
    std::array<float, 9> _sh{};
    bool _loadTried{ false };
    unsigned int _noiseId{ 0 };
    bool _noiseTried{ false };
};

} // namespace GUI
} // namespace Slic3r
