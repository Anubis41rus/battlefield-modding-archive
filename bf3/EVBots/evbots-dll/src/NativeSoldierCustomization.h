// Dedicated human soldier operations. No Lua or emulated success.
#pragma once
#include "NativeBlueprint.h"

namespace evbots { namespace soldiercustom {
struct Target {
    blueprint::Handle soldier;
    std::uintptr_t player = 0;
};
struct HumanState {
    Target target;
    float matrix[16] = {};
};
bool ReadHumanState(std::uintptr_t player, HumanState* out, const char** error);
void Start();
// Capture only on the actual engine frame. expectedBody belongs to this Lua
// soldier, never silently replaced with the player's next life.
bool CaptureHuman(std::uintptr_t player, std::uintptr_t expectedBody, Target* out, const char** error);
// ownedData means a retained native DC userdata. Untracked borrowed pointers are
// refused even on a frame: their userdata can survive an earlier level unload.
// queued=true means accepted for a later frame, not already applied.
bool Submit(const Target& target, std::uintptr_t data, bool ownedData, bool* queued, const char** error);
// Same FIFO as customization: later transforms cannot overtake earlier requests.
// Copies all four padded Vec3 rows; the actual native Spatial setter also invokes
// ServerSoldierEntity::onTransformChanged. No translation-only memory writes.
bool SubmitTransform(const Target& target, const float* matrix16, bool* queued, const char** error);
// One press for this life, consumed by the soldier's native input update.
bool RequestParachute(const Target& target, const char** error);
void Poll();
void Shutdown();
} }
