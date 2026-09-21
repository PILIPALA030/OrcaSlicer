#ifndef slic3r_PCSSShadowMath_hpp_
#define slic3r_PCSSShadowMath_hpp_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Slic3r { namespace GUI { namespace pcss {

using Vec3 = std::array<double, 3>;

// World-space lengths, including bounds and all bias/radius settings, are millimeters.
struct Bounds
{
    Vec3 min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    Vec3 max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};

    void merge(const Vec3& p)
    {
        for (unsigned i = 0; i < 3; ++i) {
            if (!std::isfinite(p[i])) {
                min[i] = max[i] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            min[i] = std::min(min[i], p[i]);
            max[i] = std::max(max[i], p[i]);
        }
    }

    bool valid() const
    {
        for (unsigned i = 0; i < 3; ++i)
            if (!std::isfinite(min[i]) || !std::isfinite(max[i]) || min[i] > max[i])
                return false;
        return true;
    }
};

struct Projection
{
    // Column-major world-to-clip matrix, OpenGL NDC [-1, 1]. No camera projection is involved.
    std::array<float, 16> matrix{};
    std::array<float, 2>  extent{};
    float                 depth_span{0.0f};
    float                 min_caster_depth{0.0f};
};

inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

inline bool normalize(Vec3& v)
{
    const double length = std::sqrt(dot(v, v));
    if (!std::isfinite(length) || length < 1.0e-12)
        return false;
    for (double& x : v)
        x /= length;
    return true;
}

inline float tan_half_angle(float full_diameter_degrees) { return std::tan(full_diameter_degrees * 0.008726646259971648f); }

inline float penumbra_radius_mm(float receiver, float blocker, float depth_span, float tangent, float limit)
{
    return std::min(std::max(0.0f, (receiver - blocker) * depth_span) * tangent, limit);
}

// Fit both domains, not the main camera frustum: offscreen geometry can cast a visible shadow.
inline bool fit_projection(
    const Bounds& casters, const Bounds& receivers, Vec3 to_light, float max_radius_mm, unsigned resolution, Projection& result)
{
    if (!casters.valid() || !receivers.valid() || !normalize(to_light) || resolution == 0 || !std::isfinite(max_radius_mm) ||
        max_radius_mm < 0.0f)
        return false;
    Vec3 right = cross(std::abs(to_light[2]) < 0.95 ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, to_light);
    if (!normalize(right))
        return false;
    const Vec3 up = cross(to_light, right);
    const Vec3 forward{-to_light[0], -to_light[1], -to_light[2]};
    Bounds     light_bounds;
    double     caster_near = std::numeric_limits<double>::infinity();
    for (const Bounds* box : {&casters, &receivers}) {
        for (unsigned corner = 0; corner < 8; ++corner) {
            Vec3 p{};
            for (unsigned axis = 0; axis < 3; ++axis)
                p[axis] = (corner & (1u << axis)) ? box->max[axis] : box->min[axis];
            const Vec3 light{dot(right, p), dot(up, p), dot(forward, p)};
            light_bounds.merge(light);
            if (box == &casters)
                caster_near = std::min(caster_near, light[2]);
        }
    }
    if (!light_bounds.valid())
        return false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const double span    = std::max(1.0, light_bounds.max[axis] - light_bounds.min[axis]);
        const double padding = axis == 2 ? std::max(1.0, span * 0.01) : max_radius_mm + 2.0 * span / resolution;
        light_bounds.min[axis] -= padding + 0.5;
        light_bounds.max[axis] += padding + 0.5;
    }
    Projection                next;
    const std::array<Vec3, 3> axes{right, up, forward};
    for (unsigned row = 0; row < 3; ++row) {
        const double span = light_bounds.max[row] - light_bounds.min[row];
        for (unsigned col = 0; col < 3; ++col)
            next.matrix[col * 4 + row] = static_cast<float>(2.0 * axes[row][col] / span);
        next.matrix[12 + row] = static_cast<float>(-(light_bounds.max[row] + light_bounds.min[row]) / span);
        if (row < 2)
            next.extent[row] = static_cast<float>(span);
        else {
            next.depth_span       = static_cast<float>(span);
            next.min_caster_depth = static_cast<float>((caster_near - light_bounds.min[row]) / span);
        }
    }
    next.matrix[15] = 1.0f;
    for (float v : next.matrix)
        if (!std::isfinite(v))
            return false;
    if (!std::isfinite(next.depth_span) || next.depth_span <= 0.0f || !std::isfinite(next.extent[0]) || !std::isfinite(next.extent[1]))
        return false;
    result = next;
    return true;
}

// Hash value fields only, never struct padding, borrowed pointers or transient GL object names.
class Signature
{
    std::uint64_t m_value{14695981039346656037ull};

public:
    void add(std::uint64_t value)
    {
        for (unsigned i = 0; i < 8; ++i) {
            m_value ^= (value >> (i * 8)) & 255u;
            m_value *= 1099511628211ull;
        }
    }
    void add_real(double value)
    {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "Expected a 64-bit double");
        std::memcpy(&bits, &value, sizeof(bits));
        add(bits);
    }
    std::uint64_t value() const { return m_value; }
};

}}} // namespace Slic3r::GUI::pcss
#endif
