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

    /// @brief Load the color panorama of the skybox once, needs a current GL context.
    /// @return true if the skybox texture is ready to bind.
    bool EnsureSkyboxLoaded();

    /// @brief Get the GL texture of the skybox panorama (with mipmaps).
    /// @return texture id, 0 if not loaded.
    unsigned int GetSkyboxTextureId() const { return _skyboxId; }

private:
    PbrEnvironment() = default;

    /// @brief Read and validate the baked file, then upload all mips.
    /// @param path full path of the baked file.
    /// @return false if the file is missing or invalid.
    bool LoadAndUpload(const std::string& path);

    /// @brief Load the skybox png and upload it with box filtered mipmaps.
    /// @param path full path of the png.
    /// @return false if the image can not be loaded.
    bool LoadSkybox(const std::string& path);

    // GL objects live until exit, the shared context is gone before static destructors run.
    unsigned int _textureId{ 0 };
    float _maxLod{ 0.0f };
    std::array<float, 9> _sh{};
    bool _loadTried{ false };
    unsigned int _skyboxId{ 0 };
    bool _skyboxTried{ false };
};

} // namespace GUI
} // namespace Slic3r
