// Native SpatialEntity transform operations; requires a tracked weak entity handle.
#pragma once
#include "NativeBlueprint.h"

namespace evbots { namespace spatial {
// Four padded Vec3 rows. Results are copied only after a successful native getter.
// Both calls use Blueprint's real frame/TLS/lifetime/context guard. They do not
// enqueue work, expose raw engine pointers or silently mutate a stale entity.
bool GetTransform(const blueprint::Handle& entity, float* transform16, const char** error);
// Calls the verified virtual setter, including the native onTransformChanged
// callback. EVBOTS_SPATIAL_TRANSFORM=0 disables this write, but not GetTransform.
// Non-finite XYZ is rejected; scale and zero axes used by BR markers are allowed.
bool SetTransform(const blueprint::Handle& entity, const float* transform16, const char** error);
} }
