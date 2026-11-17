// Runtime visual state views: these objects are not Frostbite DataContainers.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace evbots { namespace visual {
enum class Kind { Float, Boolean, Integer, Vec2, Vec3, Vec4, Asset };
struct Field { const char* name; Kind kind; unsigned offset; };
struct Component { const char* name; unsigned stateOffset; const Field* fields; unsigned count; };
struct Handle { std::uintptr_t manager=0,state=0; std::uint64_t generation=0; int id=0; };
struct State { Handle handle; std::string name; };
struct Value { float number[4]={}; int integer=0; std::uintptr_t asset=0; };

// Install at client Lua registration, before entities are initialized. All code
// signatures are checked before either hook is installed. No game launch here.
bool Initialize(const char** error);
// Reads/writes below require the real client frame, and lock the native manager.
bool GetStates(std::vector<State>* states,const char** error);
bool GetDirty(bool* dirty,const char** error);
bool SetDirty(bool dirty,const char** error);
const Field* StateField(const char* name);
const Component* FindComponent(const char* name);
const Field* FindField(const Component* component,const char* name);
bool HasComponent(const Handle& state,const Component* component,bool* present,const char** error);
// Asset results transfer a retained DataContainer reference to the caller.
bool Read(const Handle& state,const Component* component,const Field* field,Value* value,const char** error);
bool WriteState(const Handle& state,const Field* field,const Value& value,const char** error);
bool WriteComponent(const Handle& state,const Component* component,const Field* field,const Value& value,const char** error);
// VEXT 10B610F0/10B61120 and 10B614B0/10B614E0: rotation X/Y, not offsets.
bool ReadSun(unsigned axis,float* value,const char** error);
bool WriteSun(unsigned axis,float value,const char** error);
} }
