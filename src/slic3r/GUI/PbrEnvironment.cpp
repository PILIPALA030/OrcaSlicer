#include <GL/glew.h>

#include "PbrEnvironment.hpp"

#include "3DScene.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

const char* const PBR_ENVIRONMENT_FILE = "/materials/pbr_environment.bin";
const char PBR_ENVIRONMENT_MAGIC[8] = { 'P', 'B', 'R', 'E', 'N', 'V', '1', '\0' };
const uint32_t PBR_ENVIRONMENT_VERSION = 2;
// RGB specular cube map, the diffuse SH is luminance only.
const uint32_t PBR_ENVIRONMENT_CHANNELS = 3;
const uint32_t PBR_MAX_FACE_SIZE = 1024;
const uint32_t PBR_MAX_MIP_COUNT = 11;
const int PBR_CUBE_FACES = 6;

template<typename T>
bool ReadValue(boost::nowide::ifstream& file, T& value)
{
    file.read(reinterpret_cast<char*>(&value), sizeof(T));
    return file.good() && file.gcount() == static_cast<std::streamsize>(sizeof(T));
}

// Tileable noise volume of the material surface pattern; size, voxels per unit and packing match gouraud.fs.
const int PBR_NOISE_SIZE = 64;
const float PBR_NOISE_VOXELS_PER_UNIT = 2.0f;
const int PBR_NOISE_BLUR_RADIUS = 3;
const float PBR_NOISE_BLUR_SIGMA = 1.0f;
// The shader's two octave sum then has std 0.136, which the speckle coverage fit assumes.
const float PBR_NOISE_STD = 0.206f;
const float PBR_NOISE_VALUE_PACK = 0.5f;
const float PBR_NOISE_GRADIENT_RANGE = 1.5f;
// Fixed seed and mt19937 raw output keep the pattern identical on every platform.
const uint32_t PBR_NOISE_SEED = 20261030u;

size_t NoiseIndex(const int x, const int y, const int z)
{
    const int n = PBR_NOISE_SIZE;
    const size_t wx = static_cast<size_t>((x % n + n) % n);
    const size_t wy = static_cast<size_t>((y % n + n) % n);
    const size_t wz = static_cast<size_t>((z % n + n) % n);
    return (wz * static_cast<size_t>(n) + wy) * static_cast<size_t>(n) + wx;
}

/// @brief Periodic blur along one axis.
/// @param axis 0 = x, 1 = y, 2 = z.
void BlurNoiseAxis(const std::vector<float>& src, std::vector<float>& dst, const int axis, const std::vector<float>& kernel)
{
    for (int z = 0; z < PBR_NOISE_SIZE; ++z)
        for (int y = 0; y < PBR_NOISE_SIZE; ++y)
            for (int x = 0; x < PBR_NOISE_SIZE; ++x)
            {
                float sum = 0.0f;
                for (int k = -PBR_NOISE_BLUR_RADIUS; k <= PBR_NOISE_BLUR_RADIUS; ++k)
                {
                    const size_t index = NoiseIndex(x + (axis == 0 ? k : 0), y + (axis == 1 ? k : 0), z + (axis == 2 ? k : 0));
                    sum += kernel[static_cast<size_t>(k + PBR_NOISE_BLUR_RADIUS)] * src[index];
                }
                dst[NoiseIndex(x, y, z)] = sum;
            }
}

uint8_t PackUnit(const float value)
{
    const float clamped = std::min(std::max(value, 0.0f), 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

/// @brief Gaussian blurred white noise normalized to mean 0.5, with its gradient per noise unit.
/// @return RGBA8 texels: r = 0.5 + (value - 0.5) * pack, gba = 0.5 + gradient / (2 * range).
std::vector<uint8_t> BuildNoiseTexels()
{
    const size_t count = static_cast<size_t>(PBR_NOISE_SIZE) * PBR_NOISE_SIZE * PBR_NOISE_SIZE;
    std::mt19937 random(PBR_NOISE_SEED);
    std::vector<float> field(count);
    for (float& value : field)
        value = static_cast<float>(static_cast<double>(random()) / 4294967295.0 * 2.0 - 1.0);

    std::vector<float> kernel;
    float kernelSum = 0.0f;
    for (int k = -PBR_NOISE_BLUR_RADIUS; k <= PBR_NOISE_BLUR_RADIUS; ++k)
    {
        const float t = static_cast<float>(k) / PBR_NOISE_BLUR_SIGMA;
        kernel.push_back(std::exp(-0.5f * t * t));
        kernelSum += kernel.back();
    }
    for (float& weight : kernel)
        weight /= kernelSum;

    std::vector<float> blurred(count);
    for (int axis = 0; axis < 3; ++axis)
    {
        BlurNoiseAxis(field, blurred, axis, kernel);
        field.swap(blurred);
    }

    double mean = 0.0;
    for (const float value : field)
        mean += value;
    mean /= static_cast<double>(count);
    double variance = 0.0;
    for (const float value : field)
        variance += (value - mean) * (value - mean);
    const double deviation = std::sqrt(variance / static_cast<double>(count));
    const double scale = deviation > 0.0 ? PBR_NOISE_STD / deviation : 0.0;
    for (float& value : field)
        value = 0.5f + static_cast<float>((value - mean) * scale);

    std::vector<uint8_t> texels(count * 4);
    const float gradientScale = 0.5f * PBR_NOISE_VOXELS_PER_UNIT / (2.0f * PBR_NOISE_GRADIENT_RANGE);
    for (int z = 0; z < PBR_NOISE_SIZE; ++z)
        for (int y = 0; y < PBR_NOISE_SIZE; ++y)
            for (int x = 0; x < PBR_NOISE_SIZE; ++x)
            {
                const size_t index = NoiseIndex(x, y, z);
                uint8_t* texel = texels.data() + index * 4;
                texel[0] = PackUnit(0.5f + (field[index] - 0.5f) * PBR_NOISE_VALUE_PACK);
                // Central differences, scaled from per voxel to per noise unit.
                texel[1] = PackUnit(0.5f + (field[NoiseIndex(x + 1, y, z)] - field[NoiseIndex(x - 1, y, z)]) * gradientScale);
                texel[2] = PackUnit(0.5f + (field[NoiseIndex(x, y + 1, z)] - field[NoiseIndex(x, y - 1, z)]) * gradientScale);
                texel[3] = PackUnit(0.5f + (field[NoiseIndex(x, y, z + 1)] - field[NoiseIndex(x, y, z - 1)]) * gradientScale);
            }
    return texels;
}

} // namespace

PbrEnvironment& PbrEnvironment::Instance()
{
    static PbrEnvironment environment;
    return environment;
}

Matrix3f PbrEnvironment::ViewToEnvironment(const Transform3d& viewMatrix)
{
    // World (x, y, z) -> environment (x, z, -y): world up becomes the environment up axis.
    Matrix3d worldToEnvironment;
    worldToEnvironment << 1.0, 0.0, 0.0,
                          0.0, 0.0, 1.0,
                          0.0, -1.0, 0.0;
    // The view rotation is orthonormal, its inverse is the transpose.
    const Matrix3d viewRotation = viewMatrix.matrix().block<3, 3>(0, 0);
    return (worldToEnvironment * viewRotation.transpose()).cast<float>();
}

bool PbrEnvironment::EnsureLoaded()
{
    if (_textureId != 0)
        return true;
    if (_loadTried)
        return false;

    _loadTried = true;
    const std::string path = resources_dir() + PBR_ENVIRONMENT_FILE;
    if (!LoadAndUpload(path))
    {
        BOOST_LOG_TRIVIAL(warning) << "PBR environment unavailable, using fallback ambient: " << path;
        return false;
    }
    return true;
}

bool PbrEnvironment::EnsureNoiseLoaded()
{
    if (_noiseId != 0)
        return true;
    if (_noiseTried)
        return false;

    _noiseTried = true;
    if (!CreateNoiseTexture())
    {
        BOOST_LOG_TRIVIAL(warning) << "PBR noise texture unavailable, material surface patterns are disabled";
        return false;
    }
    return true;
}

bool PbrEnvironment::CreateNoiseTexture()
{
    const std::vector<uint8_t> texels = BuildNoiseTexels();
    glsafe(::glGenTextures(1, &_noiseId));
    if (_noiseId == 0)
        return false;

    GLint previousAlignment = 4;
    glsafe(::glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glBindTexture(GL_TEXTURE_3D, _noiseId));
    glsafe(::glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, PBR_NOISE_SIZE, PBR_NOISE_SIZE, PBR_NOISE_SIZE, 0, GL_RGBA,
                          GL_UNSIGNED_BYTE, texels.data()));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT));
    glsafe(::glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT));
    // Averaged mips fade sub-pixel relief out instead of letting it alias.
    glsafe(::glGenerateMipmap(GL_TEXTURE_3D));
    glsafe(::glBindTexture(GL_TEXTURE_3D, 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment));
    return true;
}

bool PbrEnvironment::LoadAndUpload(const std::string& path)
{
    boost::nowide::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
        return false;

    char magic[8] = {};
    file.read(magic, sizeof(magic));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(magic)))
        return false;
    for (size_t i = 0; i < sizeof(magic); ++i)
    {
        if (magic[i] != PBR_ENVIRONMENT_MAGIC[i])
            return false;
    }

    uint32_t version = 0;
    uint32_t baseSize = 0;
    uint32_t mipCount = 0;
    uint32_t channels = 0;
    if (!ReadValue(file, version) || !ReadValue(file, baseSize) || !ReadValue(file, mipCount) || !ReadValue(file, channels))
        return false;
    if (version != PBR_ENVIRONMENT_VERSION || channels != PBR_ENVIRONMENT_CHANNELS || baseSize == 0 || baseSize > PBR_MAX_FACE_SIZE ||
        mipCount == 0 || mipCount > PBR_MAX_MIP_COUNT || (baseSize >> (mipCount - 1)) == 0)
        return false;

    std::array<float, 9> sh{};
    for (float& value : sh)
    {
        if (!ReadValue(file, value))
            return false;
    }

    size_t texelCount = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip)
    {
        const size_t size = static_cast<size_t>(baseSize >> mip);
        texelCount += PBR_CUBE_FACES * size * size;
    }
    std::vector<uint16_t> texels(texelCount * PBR_ENVIRONMENT_CHANNELS, 0);
    const std::streamsize byteCount = static_cast<std::streamsize>(texels.size() * sizeof(uint16_t));
    file.read(reinterpret_cast<char*>(texels.data()), byteCount);
    if (file.gcount() != byteCount)
        return false;

    GLint previousAlignment = 4;
    glsafe(::glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 2));
    glsafe(::glGenTextures(1, &_textureId));
    glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, _textureId));

    size_t offset = 0;
    for (uint32_t mip = 0; mip < mipCount; ++mip)
    {
        const GLsizei size = static_cast<GLsizei>(baseSize >> mip);
        for (int face = 0; face < PBR_CUBE_FACES; ++face)
        {
            glsafe(::glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, static_cast<GLint>(mip), GL_RGB16F, size, size, 0,
                                  GL_RGB, GL_HALF_FLOAT, texels.data() + offset));
            offset += static_cast<size_t>(size) * static_cast<size_t>(size) * PBR_ENVIRONMENT_CHANNELS;
        }
    }

    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(mipCount - 1)));
    glsafe(::glBindTexture(GL_TEXTURE_CUBE_MAP, 0));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, previousAlignment));

    // Blend across cube faces at rough mips; core since GL 3.2, otherwise the face seams stay visible.
    if (GLEW_VERSION_3_2 || GLEW_ARB_seamless_cube_map)
        glsafe(::glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS));

    _sh = sh;
    _maxLod = static_cast<float>(mipCount - 1);
    return true;
}

} // namespace GUI
} // namespace Slic3r
