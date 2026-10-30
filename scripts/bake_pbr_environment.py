"""Bake an equirectangular Radiance HDR into the PBR environment used by the 140 gouraud shader.

Source of resources/materials/pbr_environment.bin:
    Ferndale Studio 03 (4k), Poly Haven, CC0: https://polyhaven.com/a/ferndale_studio_03

Usage:
    python -I scripts/bake_pbr_environment.py <input.hdr> <output.bin>

Output (little endian):
    char[8]  magic "PBRENV1\\0"
    uint32   version (2), base face size, mip count, channel count (3)
    float32  sh[9]   diffuse SH coefficients, sum(sh[i] * Y_i(n)) = irradiance(n) / pi
    float16  faces   for each mip, for each GL face (+X -X +Y -Y +Z -Z): size * size RGB texels, row by row

The diffuse SH uses luminance only so filament colors are not tinted; the specular cube map keeps the
color and the shader blends it with its luminance by a saturation setting. Everything is scaled so the
average diffuse lighting equals TARGET_DIFFUSE. Directions use the environment convention
(x right, y up, +z panorama center).
"""

import argparse
import struct
import sys

import numpy as np

MAGIC = b"PBRENV1\0"
VERSION = 2
LUMINANCE = np.array([0.2126, 0.7152, 0.0722])
BASE_SIZE = 128
MIP_COUNT = 5
SAMPLE_COUNT = 1024
TARGET_DIFFUSE = 0.7


def read_hdr(path):
    """Read a Radiance .hdr (RGBE, new style RLE or flat) into a float32 array (H, W, 3)."""
    with open(path, "rb") as file:
        data = file.read()

    pos = 0
    width = height = 0
    while True:
        end = data.index(b"\n", pos)
        line = data[pos:end].decode("ascii", "replace").strip()
        pos = end + 1
        if line.startswith("FORMAT=") and line != "FORMAT=32-bit_rle_rgbe":
            raise ValueError("unsupported format: " + line)
        if line.startswith("-Y "):
            parts = line.split()
            if len(parts) != 4 or parts[2] != "+X":
                raise ValueError("unsupported orientation: " + line)
            height, width = int(parts[1]), int(parts[3])
            break
    if width <= 0 or height <= 0:
        raise ValueError("invalid image size")

    rgbe = np.empty((height, width, 4), dtype=np.uint8)
    for row in range(height):
        if data[pos] == 2 and data[pos + 1] == 2 and (data[pos + 2] << 8 | data[pos + 3]) == width:
            pos += 4
            for channel in range(4):
                x = 0
                out = rgbe[row, :, channel]
                while x < width:
                    count = data[pos]
                    pos += 1
                    if count > 128:
                        count -= 128
                        out[x:x + count] = data[pos]
                        pos += 1
                    else:
                        out[x:x + count] = np.frombuffer(data, np.uint8, count, pos)
                        pos += count
                    x += count
        else:
            rgbe[row] = np.frombuffer(data, np.uint8, width * 4, pos).reshape(width, 4)
            pos += width * 4

    exponent = rgbe[:, :, 3].astype(np.int32)
    scale = np.where(exponent > 0, np.ldexp(1.0, exponent - 136), 0.0).astype(np.float32)
    return (rgbe[:, :, :3].astype(np.float32) + 0.5) * scale[:, :, None] * (exponent[:, :, None] > 0)


def build_pyramid(image):
    """Box filtered equirect pyramid, level 0 is the full image."""
    levels = [image]
    while levels[-1].shape[1] > 32 and levels[-1].shape[0] % 2 == 0 and levels[-1].shape[1] % 2 == 0:
        last = levels[-1]
        levels.append(0.25 * (last[0::2, 0::2] + last[1::2, 0::2] + last[0::2, 1::2] + last[1::2, 1::2]))
    return levels


def sample_equirect(image, dirs):
    """Bilinear lookup of directions (..., 3) in an equirect image (H, W) or (H, W, C), u wraps, v clamps."""
    height, width = image.shape[:2]
    x, y, z = dirs[..., 0], dirs[..., 1], dirs[..., 2]
    phi = np.arctan2(x, z)
    theta = np.arcsin(np.clip(y, -1.0, 1.0))
    u = (phi / (2.0 * np.pi) + 0.5) * width - 0.5
    v = np.clip((0.5 - theta / np.pi) * height - 0.5, 0.0, height - 1.0)
    u0 = np.floor(u).astype(np.int64)
    v0 = np.floor(v).astype(np.int64)
    fu = u - u0
    fv = v - v0
    if image.ndim == 3:
        fu = fu[..., None]
        fv = fv[..., None]
    u1 = (u0 + 1) % width
    u0 = u0 % width
    v1 = np.minimum(v0 + 1, height - 1)
    top = image[v0, u0] * (1.0 - fu) + image[v0, u1] * fu
    bottom = image[v1, u0] * (1.0 - fu) + image[v1, u1] * fu
    return top * (1.0 - fv) + bottom * fv


def face_directions(face, size):
    """Normalized texel center directions of a GL cube face, shape (size, size, 3), row = t."""
    coords = (np.arange(size, dtype=np.float64) + 0.5) / size * 2.0 - 1.0
    tc, sc = np.meshgrid(coords, coords, indexing="ij")
    one = np.ones_like(sc)
    # GL cube map face table: (direction for given sc, tc).
    table = [
        (one, -tc, -sc),   # +X
        (-one, -tc, sc),   # -X
        (sc, one, tc),     # +Y
        (sc, -one, -tc),   # -Y
        (sc, -tc, one),    # +Z
        (-sc, -tc, -one),  # -Z
    ]
    dirs = np.stack(table[face], axis=-1)
    return dirs / np.linalg.norm(dirs, axis=-1, keepdims=True)


def hammersley(count):
    bits = np.arange(count, dtype=np.uint32)
    bits = ((bits << 16) | (bits >> 16)) & 0xFFFFFFFF
    bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1)
    bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2)
    bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4)
    bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8)
    return np.arange(count) / count, bits.astype(np.float64) / 4294967296.0


def ggx_samples(roughness, count, source_texel_solid_angle):
    """Tangent space reflected directions (N = V), NdotL weights and source pyramid lods."""
    alpha = roughness * roughness
    xi1, xi2 = hammersley(count)
    cos_theta = np.sqrt((1.0 - xi2) / (1.0 + (alpha * alpha - 1.0) * xi2))
    sin_theta = np.sqrt(1.0 - cos_theta * cos_theta)
    phi = 2.0 * np.pi * xi1
    half_vec = np.stack([sin_theta * np.cos(phi), sin_theta * np.sin(phi), cos_theta], axis=-1)
    light = 2.0 * cos_theta[:, None] * half_vec - np.array([0.0, 0.0, 1.0])
    n_dot_l = light[:, 2]
    keep = n_dot_l > 0.0
    d = alpha * alpha / (np.pi * ((cos_theta * cos_theta) * (alpha * alpha - 1.0) + 1.0) ** 2)
    pdf = d / 4.0
    sample_solid_angle = 1.0 / (count * pdf)
    lod = np.maximum(0.5 * np.log2(sample_solid_angle / source_texel_solid_angle) + 1.0, 0.0)
    return light[keep], n_dot_l[keep], lod[keep]


def prefilter_face(pyramid, normals, light, weight, lod):
    """GGX prefiltered radiance for face normals (size, size, 3), result (size, size) + pyramid channels."""
    up = np.where(np.abs(normals[..., 1:2]) < 0.999, np.array([0.0, 1.0, 0.0]), np.array([1.0, 0.0, 0.0]))
    tangent = np.cross(up, normals)
    tangent /= np.linalg.norm(tangent, axis=-1, keepdims=True)
    bitangent = np.cross(normals, tangent)

    channels = pyramid[0].shape[2:]
    sample_shape = (1, 1, -1) + (1,) * len(channels)
    result = np.zeros(normals.shape[:-1] + channels)
    lod = np.minimum(lod, len(pyramid) - 1)
    lower = np.floor(lod).astype(int)
    for level in np.unique(lower):
        mask = lower == level
        frac = (lod[mask] - level).reshape(sample_shape)
        dirs = (tangent[..., None, :] * light[mask, 0][None, None, :, None]
                + bitangent[..., None, :] * light[mask, 1][None, None, :, None]
                + normals[..., None, :] * light[mask, 2][None, None, :, None])
        value = sample_equirect(pyramid[level], dirs)
        if level + 1 < len(pyramid):
            value = value * (1.0 - frac) + sample_equirect(pyramid[level + 1], dirs) * frac
        result += np.sum(value * weight[mask].reshape(sample_shape), axis=2)
    return result / np.sum(weight)


def mirror_face(pyramid, face, size):
    """Roughness 0 level: pyramid level close to the cube texel size, 2x2 supersampled per texel."""
    level = int(np.clip(np.round(np.log2(pyramid[0].shape[1] / (4.0 * size))), 0, len(pyramid) - 1))
    fine = sample_equirect(pyramid[level], face_directions(face, 2 * size))
    return 0.25 * (fine[0::2, 0::2] + fine[1::2, 0::2] + fine[0::2, 1::2] + fine[1::2, 1::2])


def sh9_basis(dirs):
    x, y, z = dirs[..., 0], dirs[..., 1], dirs[..., 2]
    return np.stack([
        np.full_like(x, 0.282095),
        0.488603 * y, 0.488603 * z, 0.488603 * x,
        1.092548 * x * y, 1.092548 * y * z, 0.315392 * (3.0 * z * z - 1.0),
        1.092548 * x * z, 0.546274 * (x * x - y * y),
    ], axis=-1)


def diffuse_sh(image):
    """SH9 coefficients c with sum(c_i * Y_i(n)) = irradiance(n) / pi."""
    height, width = image.shape
    lat = (0.5 - (np.arange(height) + 0.5) / height) * np.pi
    phi = ((np.arange(width) + 0.5) / width - 0.5) * 2.0 * np.pi
    lat, phi = np.meshgrid(lat, phi, indexing="ij")
    dirs = np.stack([np.cos(lat) * np.sin(phi), np.sin(lat), np.cos(lat) * np.cos(phi)], axis=-1)
    solid_angle = (2.0 * np.pi / width) * (np.pi / height) * np.cos(lat)
    radiance_sh = np.einsum("hw,hwk->k", image * solid_angle, sh9_basis(dirs))
    band = np.array([np.pi, 2.0 * np.pi / 3.0, 2.0 * np.pi / 3.0, 2.0 * np.pi / 3.0,
                     np.pi / 4.0, np.pi / 4.0, np.pi / 4.0, np.pi / 4.0, np.pi / 4.0])
    return radiance_sh * band / np.pi


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", help="equirectangular Radiance .hdr")
    parser.add_argument("output", help="output .bin")
    args = parser.parse_args()

    rgb = read_hdr(args.input)
    pyramid = build_pyramid(rgb)
    print("source %dx%d, pyramid levels %d" % (rgb.shape[1], rgb.shape[0], len(pyramid)))

    # Diffuse uses luminance only, so the studio color does not tint filament colors.
    sh_level = next(level for level in pyramid if level.shape[1] <= 256)
    sh = diffuse_sh(sh_level.astype(np.float64) @ LUMINANCE)
    scale = TARGET_DIFFUSE / (sh[0] * 0.282095)
    sh *= scale
    pyramid = [level * np.float32(scale) for level in pyramid]
    print("scale %.5f, peak radiance %.2f" % (scale, pyramid[0].max()))
    for name, normal in (("up", (0, 1, 0)), ("down", (0, -1, 0)), ("camera", (0, 0, 1)), ("left", (-1, 0, 0))):
        print("diffuse %-6s %.3f" % (name, float(sh9_basis(np.array(normal, dtype=np.float64)) @ sh)))

    source_texel_solid_angle = 4.0 * np.pi / (pyramid[0].shape[0] * pyramid[0].shape[1])
    blob = bytearray(struct.pack("<8sIIII", MAGIC, VERSION, BASE_SIZE, MIP_COUNT, 3))
    blob += struct.pack("<9f", *[float(value) for value in sh])
    for mip in range(MIP_COUNT):
        size = BASE_SIZE >> mip
        roughness = mip / (MIP_COUNT - 1)
        if mip > 0:
            light, weight, lod = ggx_samples(roughness, SAMPLE_COUNT, source_texel_solid_angle)
        for face in range(6):
            if mip == 0:
                texels = mirror_face(pyramid, face, size)
            else:
                texels = prefilter_face(pyramid, face_directions(face, size), light, weight, lod)
            blob += np.clip(texels, 0.0, 65000.0).astype("<f2").tobytes()
        print("mip %d: %dx%d roughness %.2f" % (mip, size, size, roughness))

    with open(args.output, "wb") as file:
        file.write(blob)
    print("wrote %s (%d bytes)" % (args.output, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
