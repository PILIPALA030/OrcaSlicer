#include "PCSSShadowMath.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace Slic3r::GUI::pcss;

namespace {
unsigned assertions = 0;
void     check(bool condition, const char* message)
{
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}
Vec3 project(const Projection& p, Vec3 v)
{
    Vec3 out{};
    for (unsigned row = 0; row < 3; ++row) {
        out[row] = p.matrix[12 + row];
        for (unsigned col = 0; col < 3; ++col)
            out[row] += p.matrix[4 * col + row] * v[col];
    }
    return out;
}
} // namespace

int main()
{
    try {
        check(!Bounds{}.valid(), "Empty bounds must be invalid");
        const Bounds casters{{-30, -20, 0}, {50, 70, 80}};
        const Bounds receivers{{-100, -100, 0}, {100, 100, 0}};
        for (Vec3 light : {Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{0, -1, 0}, Vec3{-0.6, 0.6, 1}, Vec3{0, 0, -1}}) {
            Projection p;
            check(fit_projection(casters, receivers, light, 12, 2048, p), "Projection fit failed");
            check(p.extent[0] > 0 && p.extent[1] > 0 && p.depth_span > 0, "Invalid metric spans");
            for (const Bounds* bounds : {&casters, &receivers}) {
                for (unsigned corner = 0; corner < 8; ++corner) {
                    Vec3 v{};
                    for (unsigned axis = 0; axis < 3; ++axis)
                        v[axis] = (corner & (1u << axis)) ? bounds->max[axis] : bounds->min[axis];
                    const Vec3 clip = project(p, v);
                    check(std::abs(clip[0]) < 1 && std::abs(clip[1]) < 1 && std::abs(clip[2]) < 1, "Corner clipped");
                }
            }
            normalize(light);
            const Vec3 closer{10 * light[0], 10 * light[1], 10 * light[2]};
            check(project(p, closer)[2] < project(p, {0, 0, 0})[2], "Depth direction reversed");
            check(std::abs((project(p, {0, 0, 0})[2] - project(p, closer)[2]) * 0.5 * p.depth_span - 10) < 1e-4,
                  "Encoded depth difference must restore millimeters");
        }
        Bounds invalid_bounds = casters;
        invalid_bounds.merge({NAN, 0, 0});
        check(!invalid_bounds.valid(), "Nonfinite source points cannot be silently ignored");
        Projection p;
        check(!fit_projection({}, receivers, {0, 0, 1}, 12, 2048, p), "Reject empty casters");
        check(!fit_projection(casters, receivers, {0, 0, 0}, 12, 2048, p), "Reject zero light direction");
        check(!fit_projection(casters, receivers, {0, 0, 1}, -1, 2048, p), "Reject negative radius");
        check(!fit_projection(casters, receivers, {0, 0, 1}, 12, 0, p), "Reject zero resolution");
        check(!fit_projection(casters, receivers, {NAN, 0, 1}, 12, 2048, p), "Reject nonfinite direction");
        check(fit_projection({{0, 0, 0}, {0, 0, 0}}, {{0, 0, 0}, {0, 0, 0}}, {0, 0, 1}, 1, 256, p),
              "Finite degenerate domain needs a safe fitted volume");
        check(std::abs(penumbra_radius_mm(0.5f, 0.2f, 200, 0.05f, 12) - 3) < 1e-5, "Metric penumbra example");
        check(std::abs(penumbra_radius_mm(0.6f, 0.45f, 400, 0.05f, 12) - 3) < 1e-5, "Encoding invariance");
        check(penumbra_radius_mm(0.2f, 0.5f, 200, 0.05f, 12) == 0, "Negative gaps clamp");
        check(penumbra_radius_mm(0.5f, 0.2f, 200, 0.05f, 2) == 2, "Maximum radius clamp");
        check(penumbra_radius_mm(0.5f, 0.5f, 200, 0.05f, 12) == 0, "Contact is hard");
        check(tan_half_angle(0) == 0, "Zero angular diameter is hard");
        check(std::abs(tan_half_angle(90) - 1) < 1e-6, "Full diameter, not half angle, input");
        Signature a, b, c;
        a.add(42);
        a.add_real(1.25);
        b.add(42);
        b.add_real(1.25);
        c.add(42);
        c.add_real(1.5);
        check(a.value() == b.value(), "Stable cache signature");
        check(a.value() != c.value(), "World transform changes invalidate signature");
        std::cout << "PCSS math: " << assertions << " assertions passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
