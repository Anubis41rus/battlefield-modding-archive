// VextSpatial.h — SpatialEntity и AxisAlignedBox (stub) для EVBots.
//
// Реализация SpatialEntity ВСТАВЛЯЕТСЯ В VextGeometry.cpp (через общий Lua-источник).
// Этот заголовок только объявляет точку входа для регистрации.
//
// SpatialEntity — обёртка Entity для доступа к позиции/трансформу.
// В реальном VU это нативный тип; в EVBots возвращает stub-данные
// (нулевые координаты), т.к. botlayer.dll не предоставляет С++-доступа
// к позициям сущностей без отправки в Lua.
//
// Документация: content\vext\ref\shared\type\spatialentity.md
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать SpatialEntity, SpatialTransform (внутренний) и
/// AxisAlignedBox в состоянии Lua.
/// Вызывать после RegisterVextGeometry (зависит от Vec3/LinearTransform).
bool RegisterVextSpatial(lua_State* L);

}  // namespace evbots
