#pragma once
struct lua_State;
namespace evbots {
// Register after RegisterVextRcon. No engine patch until a native setter runs.
bool RegisterNativeRconGameplay(lua_State* L);
// Client startup gate for the narrowly supported stock squad-size four.
bool VerifyNativeSquadSizeClient();
}
