#include <GL/glew.h>

#include "PbrEnvironment.hpp"

#include "3DScene.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/image.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

const char* const PBR_ENVIRONMENT_FILE = "/materials/pbr_environment.bin";
const char* const PBR_SKYBOX_FILE = "/materials/pbr_skybox.png";
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

bool PbrEnvironment::EnsureSkyboxLoaded()
{
    if (_skyboxId != 0)
        return true;
    if (_skyboxTried)
        return false;

    _skyboxTried = true;
    const std::string path = resources_dir() + PBR_SKYBOX_FILE;
    if (!LoadSkybox(path))
    {
        BOOST_LOG_TRIVIAL(warning) << "PBR skybox unavailable, using the plain background: " << path;
        return false;
    }
    return true;
}

bool PbrEnvironment::LoadSkybox(const std::string& path)
{
    wxImage image;
    if (!image.LoadFile(wxString::FromUTF8(path.c_str()), wxBITMAP_TYPE_PNG) || !image.IsOk())
        return false;
    int width = image.GetWidth();
    int height = image.GetHeight();
    if (width <= 0 || height <= 0 || image.GetData() == nullptr)
        return false;

    GLint previousAlignment = 4;
    glsafe(::glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousAlignment));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    glsafe(::glGenTextures(1, &_skyboxId));
    glsafe(::glBindTexture(GL_TEXTURE_2D, _skyboxId));

    // Box filtered mipmaps, the shader blurs the background by sampling a coarser level.
    GLint level = 0;
    while (true)
    {
        glsafe(::glTexImage2D(GL_TEXTURE_2D, level, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, image.GetData()));
        if (width == 1 && height == 1)
            break;
        width = std::max(width / 2, 1);
        height = std::max(height / 2, 1);
        image = image.ResampleBox(width, height);
        if (!image.IsOk() || image.GetData() == nullptr)
            break;
        ++level;
    }

    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    // The panorama wraps horizontally.
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level));
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
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
