#pragma once

#include <map>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

/// @brief PBR (metallic-roughness) parameters of one filament.
struct PbrMaterialParams
{
    float metallic{ 0.0f };
    float roughness{ 0.55f };
    // Procedural surface pattern in model space (mm), no textures; 0 disables it.
    float colorVariation{ 0.0f };
    float variationScale{ 8.0f };
    float speckle{ 0.0f };
    float speckleSize{ 0.4f };
    float speckleDarkness{ 0.5f };
    // Procedural micro relief: grain slope strength and grain size (mm); speckles get raised by it too.
    float bump{ 0.0f };
    float bumpScale{ 0.25f };
    // Relief shape: 0 = smooth waves, 1 = packed grains (sandblasted / matte).
    float bumpGrain{ 0.0f };
    // Rough surface retro reflection (Burley diffuse): brighter, softer edges of matte filaments.
    float sheen{ 0.0f };
};

/// @brief Description of one material parameter: json key, shader uniform, member and range.
struct PbrMaterialField
{
    const char* key;
    const char* uniform;
    float PbrMaterialParams::*member;
    float minValue;
    float maxValue;
    // Written to json even when equal to the default value.
    bool alwaysWrite;
    // Short help shown after the debug panel slider.
    const char* description;
};

/// @brief Get all material parameters, shared by json io, shader uniforms and the debug panel.
/// @return the field list.
const std::vector<PbrMaterialField>& GetPbrMaterialFields();

/// @brief Image based lighting of the PBR shader.
struct PbrLightingParams
{
    float exposure{ 1.0f };
    // Baked environment average diffuse is 0.7; the default rotation lights the printer front view from the upper left.
    float envIntensity{ 0.5f };
    float envRotation{ 150.0f };
    float diffuse{ 1.0f };
    float reflection{ 1.0f };
    // 0 = gray reflections, 1 = studio colors; diffuse always stays gray to keep filament colors.
    float reflectionSaturation{ 0.0f };
    // Build plate surface, textured PEI by default.
    float bedMetallic{ 0.0f };
    float bedRoughness{ 0.6f };
};

/// @brief Description of one lighting parameter: json key, shader uniform, member and range.
struct PbrLightingField
{
    const char* key;
    const char* uniform;
    float PbrLightingParams::*member;
    float minValue;
    float maxValue;
    // Short help shown after the debug panel slider.
    const char* description;
};

/// @brief Get all lighting parameters, shared by json io, shader uniforms and the debug panel.
/// @return the field list.
const std::vector<PbrLightingField>& GetPbrLightingFields();

/// @brief Section of the material table that matched a filament.
enum class PbrMatchSource
{
    Name,
    Type,
    Default
};

/// @brief Material lookup result of one filament.
struct PbrMaterialMatch
{
    PbrMatchSource source{ PbrMatchSource::Default };
    // Matched key in its section, empty for the default material.
    std::string key;
    // Base preset name without printer / nozzle suffix.
    std::string filamentName;
    PbrMaterialParams params;
};

/// @brief Constant PBR material table stored in resources/materials/pbr_presets.json.
class PbrMaterialTable
{
public:
    /// @brief Get the table, loaded on first call. Main thread only.
    /// @return the shared table instance.
    static PbrMaterialTable& Instance();

    /// @brief Find material: preset name -> exact type -> type suffix (e.g. "-CF") -> base type -> default.
    /// @param filamentName base preset name, may be empty.
    /// @param filamentType value of the filament_type option, may be empty or user defined.
    /// @return the match, the default material if nothing matches.
    PbrMaterialMatch Find(const std::string& filamentName, const std::string& filamentType) const;

    /// @brief Get the fallback material.
    /// @return the default material.
    const PbrMaterialParams& GetDefault() const { return _default; }

    /// @brief Get the scene lighting.
    /// @return the lighting parameters.
    const PbrLightingParams& GetLighting() const { return _lighting; }

    /// @brief Set the scene lighting in memory, values are clamped to their ranges.
    /// @param lighting new lighting parameters.
    void SetLighting(const PbrLightingParams& lighting);

    /// @brief Set an entry in memory, created if missing. Key is ignored for the default source.
    /// @param source section to edit.
    /// @param key preset name or filament type, must not be empty for name / type.
    /// @param params new parameters, clamped to the valid range.
    /// @return false if the key is invalid.
    bool SetMaterial(const PbrMatchSource source, const std::string& key, const PbrMaterialParams& params);

    /// @brief Write the table back to its json file.
    /// @return false if the file can not be written.
    bool Save();

    /// @brief Check for in-memory edits not written to the file yet.
    /// @return true if there are unsaved changes.
    bool IsDirty() const { return _dirty; }

    /// @brief Reload the table from its json file.
    /// @return false if the file is missing or invalid, the table then only has the default material.
    bool Reload();

    /// @brief Get the json file path.
    /// @return full path of the json file.
    const std::string& GetPath() const { return _path; }

private:
    PbrMaterialTable();

    /// @brief Load presets from the json file, type keys are stored in upper case.
    /// @return false if the file is missing or invalid.
    bool Load();

    /// @brief Find material by filament type: exact -> suffix -> base.
    /// @param filamentType value of the filament_type option.
    /// @param match receives the matched key and parameters.
    /// @return false if no type entry matches.
    bool FindByType(const std::string& filamentType, PbrMaterialMatch& match) const;

    std::string _path;
    std::map<std::string, PbrMaterialParams> _names;
    std::map<std::string, PbrMaterialParams> _types;
    PbrMaterialParams _default;
    PbrLightingParams _lighting;
    bool _dirty{ false };
};

} // namespace GUI
} // namespace Slic3r
