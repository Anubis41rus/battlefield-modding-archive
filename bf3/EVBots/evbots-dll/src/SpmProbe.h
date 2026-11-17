// SpmProbe.h — диагностика ServerPlayerManager (см. SpmProbe.cpp).
#pragma once

namespace evbots {

// Ставит inline-hook на ServerPlayerManager::update (RVA 0xA5EF00),
// чтобы печатать живых игроков. moduleBase = база образа сервера.
bool InstallSpmProbe(void* moduleBase);

}  // namespace evbots