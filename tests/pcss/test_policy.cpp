#include "PCSSShadowPolicy.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace Slic3r::GUI::pcss;
static void environment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
int main()
{
    try {
        unsigned checks = 0;
        auto     check  = [&](bool value) {
            ++checks;
            if (!value)
                throw std::runtime_error("PCSS policy contract");
        };
        for (const char* value : {"", "0", "false", "invalid"}) {
            environment("ORCA_PCSS_BOUNDS", value);
            check(!default_depth_bounds());
        }
        environment("ORCA_PCSS_BOUNDS", "1");
        check(default_depth_bounds());
        for (unsigned blockers = 1; blockers <= 64; ++blockers)
            for (unsigned filters = 1; filters <= 64; ++filters) {
                const auto balanced = model_sample_budget(blockers, filters, 8, 16, false);
                check(balanced.blockers == std::min(blockers, 8u) && balanced.filters == std::min(filters, 16u));
                const auto reference = model_sample_budget(blockers, filters, 8, 16, true);
                check(reference.blockers == blockers && reference.filters == filters);
            }
        std::cout << "PCSS policy: " << checks << " checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
