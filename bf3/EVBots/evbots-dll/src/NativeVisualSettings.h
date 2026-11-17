// VU-compatible visual switches; only native client effects, no server transport.
#pragma once
#include <cstdint>
namespace evbots { namespace visualsettings {
bool Initialize(const char** error);
bool SetColorCorrectionEnabled(bool enabled,const char** error);
bool SetSunFlareEnabled(bool enabled,const char** error);
bool ColorCorrectionEnabled();
bool SunFlareEnabled();
// Called ONLY by the verified native VisualEnvironmentEntity initialization hook
// after native init and state registration, before the new entity is exposed.
void OnStateCreated(std::uintptr_t state,const char* name);
} }
