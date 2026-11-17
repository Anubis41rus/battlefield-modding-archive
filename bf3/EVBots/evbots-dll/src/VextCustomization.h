// VextCustomization.h — fb-типы кастомизации солдата (VU).

// Подробности — в VextCustomization.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать fb-типы кастомизации (CustomizeSoldierData,
/// UnlockWeaponAndSlot, SoldierWeaponUnlockAsset, UnlockAsset).
/// Возвращает false, если регистрация не удалась.
bool RegisterVextCustomization(lua_State* L);

}  // namespace evbots
