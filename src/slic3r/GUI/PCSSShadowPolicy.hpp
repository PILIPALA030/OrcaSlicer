#ifndef slic3r_PCSSShadowPolicy_hpp_
#define slic3r_PCSSShadowPolicy_hpp_

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Slic3r { namespace GUI { namespace pcss {

inline bool environment_is(const char* name, const char* value)
{
    const char* setting = std::getenv(name);
    return setting != nullptr && std::strcmp(setting, value) == 0;
}

// Environment supplies defaults only. A subsequently assigned settings value remains authoritative,
// except the existing ORCA_PCSS_BOUNDS=0 diagnostic, which explicitly disables the accelerator.
inline bool default_depth_bounds() { return environment_is("ORCA_PCSS_BOUNDS", "1"); }

struct SampleBudget
{
    unsigned blockers;
    unsigned filters;
};

inline SampleBudget model_sample_budget(unsigned blockers, unsigned filters, unsigned model_blockers, unsigned model_filters, bool reference)
{
    return reference ? SampleBudget{blockers, filters} : SampleBudget{std::min(blockers, model_blockers), std::min(filters, model_filters)};
}

// Independent flags: one redraw can have multiple causes. Revision groups geometry, transforms,
// clipping and selected LOD; it is not a claim that any particular one of them changed.
enum UpdateReason : unsigned {
    CACHE_HIT         = 0,
    INVALID_MAP       = 1u,
    SCENE_REVISION    = 2u,
    LIGHT_DIRECTION   = 4u,
    FITTED_PROJECTION = 8u,
    DEPTH_PROGRAM     = 16u
};

}}} // namespace Slic3r::GUI::pcss
#endif
