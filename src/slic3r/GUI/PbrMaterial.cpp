#include "PbrMaterial.hpp"

#include "libslic3r/Utils.hpp"

#include "nlohmann/json.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

namespace Slic3r {
namespace GUI {

namespace {

const char* const PBR_PRESETS_FILE = "/materials/pbr_presets.json";
const char* const PBR_KEY_DEFAULT = "default";
const char* const PBR_KEY_NAMES = "names";
const char* const PBR_KEY_TYPES = "types";
const char* const PBR_KEY_LIGHTING = "lighting";
// Avoid GGX singularity at zero roughness.
const float PBR_MIN_ROUGHNESS = 0.045f;

float ClampValue(const float value, const float minValue)
{
    return std::min(std::max(value, minValue), 1.0f);
}

PbrMaterialParams ClampParams(const PbrMaterialParams& params)
{
    PbrMaterialParams clamped;
    clamped.metallic = ClampValue(params.metallic, 0.0f);
    clamped.roughness = ClampValue(params.roughness, PBR_MIN_ROUGHNESS);
    return clamped;
}

float ReadFloat(const nlohmann::json& item, const char* name, const float fallback)
{
    const nlohmann::json::const_iterator it = item.find(name);
    if (it == item.end() || !it->is_number())
        return fallback;
    return it->get<float>();
}

PbrMaterialParams ReadParams(const nlohmann::json& item, const PbrMaterialParams& fallback)
{
    PbrMaterialParams params;
    params.metallic = ReadFloat(item, "metallic", fallback.metallic);
    params.roughness = ReadFloat(item, "roughness", fallback.roughness);
    return ClampParams(params);
}

void ReadSection(const nlohmann::json& root, const char* name, const bool upperKeys,
                 std::map<std::string, PbrMaterialParams>& out)
{
    const nlohmann::json::const_iterator section = root.find(name);
    if (section == root.end() || !section->is_object())
        return;

    const PbrMaterialParams fallback;
    for (nlohmann::json::const_iterator it = section->begin(); it != section->end(); ++it)
    {
        if (!it->is_object() || it.key().empty())
            continue;
        const std::string key = upperKeys ? boost::algorithm::to_upper_copy(it.key()) : it.key();
        out[key] = ReadParams(*it, fallback);
    }
}

// Keep 3 decimals so the saved file stays readable.
double RoundValue(const float value)
{
    return std::round(static_cast<double>(value) * 1000.0) / 1000.0;
}

void WriteParams(std::ostringstream& out, const PbrMaterialParams& params)
{
    out << "{ \"metallic\": " << RoundValue(params.metallic) << ", \"roughness\": " << RoundValue(params.roughness) << " }";
}

PbrLightingParams ClampLighting(const PbrLightingParams& lighting)
{
    PbrLightingParams clamped = lighting;
    for (const PbrLightingField& field : GetPbrLightingFields())
        clamped.*field.member = std::min(std::max(clamped.*field.member, field.minValue), field.maxValue);
    return clamped;
}

PbrLightingParams ReadLighting(const nlohmann::json& root)
{
    PbrLightingParams lighting;
    const nlohmann::json::const_iterator section = root.find(PBR_KEY_LIGHTING);
    if (section == root.end() || !section->is_object())
        return lighting;
    for (const PbrLightingField& field : GetPbrLightingFields())
        lighting.*field.member = ReadFloat(*section, field.key, lighting.*field.member);
    return ClampLighting(lighting);
}

void WriteLighting(std::ostringstream& out, const PbrLightingParams& lighting)
{
    const std::vector<PbrLightingField>& fields = GetPbrLightingFields();
    out << "    \"" << PBR_KEY_LIGHTING << "\": {\n";
    for (size_t i = 0; i < fields.size(); ++i)
    {
        out << "        \"" << fields[i].key << "\": " << RoundValue(lighting.*fields[i].member);
        out << (i + 1 < fields.size() ? ",\n" : "\n");
    }
    out << "    },\n";
}

void WriteSection(std::ostringstream& out, const char* name, const std::map<std::string, PbrMaterialParams>& section,
                  const bool isLast)
{
    out << "    \"" << name << "\": {\n";
    size_t index = 0;
    for (const std::pair<const std::string, PbrMaterialParams>& item : section)
    {
        out << "        " << nlohmann::json(item.first).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << ": ";
        WriteParams(out, item.second);
        out << (++index < section.size() ? ",\n" : "\n");
    }
    out << (isLast ? "    }\n" : "    },\n");
}

} // namespace

const std::vector<PbrLightingField>& GetPbrLightingFields()
{
    static const std::vector<PbrLightingField> fields = {
        { "exposure",      "pbr_exposure",      &PbrLightingParams::exposure,     0.1f,    4.0f },
        { "env_intensity", "pbr_env_intensity", &PbrLightingParams::envIntensity, 0.0f,    3.0f },
        { "env_rotation",  "pbr_env_rotation",  &PbrLightingParams::envRotation,  -180.0f, 180.0f },
        { "diffuse",       "pbr_diffuse",       &PbrLightingParams::diffuse,      0.0f,    3.0f },
        { "reflection",    "pbr_reflection",    &PbrLightingParams::reflection,   0.0f,    3.0f },
        { "reflection_saturation", "pbr_reflection_saturation", &PbrLightingParams::reflectionSaturation, 0.0f, 1.0f },
        // Skybox shader uniforms, ignored by the gouraud shader.
        { "skybox_brightness", "brightness",    &PbrLightingParams::skyboxBrightness, 0.0f, 2.0f },
        { "skybox_saturation", "saturation",    &PbrLightingParams::skyboxSaturation, 0.0f, 1.0f },
        { "skybox_blur",       "blur",          &PbrLightingParams::skyboxBlur,       0.0f, 6.0f },
    };
    return fields;
}

PbrMaterialTable& PbrMaterialTable::Instance()
{
    static PbrMaterialTable table;
    return table;
}

PbrMaterialTable::PbrMaterialTable()
    : _path(resources_dir() + PBR_PRESETS_FILE)
{
    if (!Load())
        BOOST_LOG_TRIVIAL(warning) << "PBR material presets unavailable, using default material: " << _path;
}

bool PbrMaterialTable::Load()
{
    _names.clear();
    _types.clear();
    _default = PbrMaterialParams();
    _lighting = PbrLightingParams();
    _dirty = false;

    boost::nowide::ifstream file(_path);
    if (!file.is_open())
        return false;

    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const nlohmann::json root = nlohmann::json::parse(content, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;

    const nlohmann::json::const_iterator defaultIt = root.find(PBR_KEY_DEFAULT);
    if (defaultIt != root.end() && defaultIt->is_object())
        _default = ReadParams(*defaultIt, _default);

    ReadSection(root, PBR_KEY_NAMES, false, _names);
    ReadSection(root, PBR_KEY_TYPES, true, _types);
    _lighting = ReadLighting(root);
    return true;
}

void PbrMaterialTable::SetLighting(const PbrLightingParams& lighting)
{
    _lighting = ClampLighting(lighting);
    _dirty = true;
}

bool PbrMaterialTable::Reload()
{
    const bool loaded = Load();
    if (!loaded)
        BOOST_LOG_TRIVIAL(warning) << "PBR material presets reload failed: " << _path;
    return loaded;
}

bool PbrMaterialTable::Save()
{
    std::ostringstream out;
    out << "{\n    \"" << PBR_KEY_DEFAULT << "\": ";
    WriteParams(out, _default);
    out << ",\n";
    WriteLighting(out, _lighting);
    WriteSection(out, PBR_KEY_NAMES, _names, false);
    WriteSection(out, PBR_KEY_TYPES, _types, true);
    out << "}\n";

    boost::nowide::ofstream file(_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        BOOST_LOG_TRIVIAL(warning) << "PBR material presets save failed, can not open: " << _path;
        return false;
    }
    file << out.str();
    file.flush();
    if (!file.good())
    {
        BOOST_LOG_TRIVIAL(warning) << "PBR material presets save failed, write error: " << _path;
        return false;
    }
    _dirty = false;
    return true;
}

bool PbrMaterialTable::SetMaterial(const PbrMatchSource source, const std::string& key, const PbrMaterialParams& params)
{
    const PbrMaterialParams clamped = ClampParams(params);
    switch (source)
    {
    case PbrMatchSource::Name:
        if (key.empty())
            return false;
        _names[key] = clamped;
        break;
    case PbrMatchSource::Type:
        if (key.empty())
            return false;
        _types[boost::algorithm::to_upper_copy(key)] = clamped;
        break;
    case PbrMatchSource::Default:
        _default = clamped;
        break;
    default:
        return false;
    }
    _dirty = true;
    return true;
}

PbrMaterialMatch PbrMaterialTable::Find(const std::string& filamentName, const std::string& filamentType) const
{
    PbrMaterialMatch match;
    match.filamentName = filamentName;
    match.params = _default;

    const std::map<std::string, PbrMaterialParams>::const_iterator nameIt = _names.find(filamentName);
    if (!filamentName.empty() && nameIt != _names.end())
    {
        match.source = PbrMatchSource::Name;
        match.key = nameIt->first;
        match.params = nameIt->second;
        return match;
    }

    if (FindByType(filamentType, match))
        match.source = PbrMatchSource::Type;
    return match;
}

bool PbrMaterialTable::FindByType(const std::string& filamentType, PbrMaterialMatch& match) const
{
    if (filamentType.empty())
        return false;

    const std::string type = boost::algorithm::to_upper_copy(filamentType);
    std::map<std::string, PbrMaterialParams>::const_iterator found = _types.find(type);

    const std::string::size_type dashPos = type.find('-');
    if (found == _types.end() && dashPos != std::string::npos)
    {
        // Suffix keys start with '-', e.g. "-CF" also matches "PETG-CF10".
        const std::string suffix = type.substr(dashPos);
        for (std::map<std::string, PbrMaterialParams>::const_iterator it = _types.begin(); it != _types.end(); ++it)
        {
            if (it->first[0] == '-' && suffix.find(it->first) != std::string::npos)
            {
                found = it;
                break;
            }
        }
        if (found == _types.end())
            found = _types.find(type.substr(0, dashPos));
    }

    if (found == _types.end())
        return false;

    match.key = found->first;
    match.params = found->second;
    return true;
}

} // namespace GUI
} // namespace Slic3r
