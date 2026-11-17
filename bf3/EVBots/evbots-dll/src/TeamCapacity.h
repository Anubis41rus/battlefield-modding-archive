#pragma once

namespace evbots {
// Только до создания первого Level, при остановленном потоке движка.
// false: выключено EVBOTS_TEAM_CAPACITY=0, несовместимый exe либо ошибка установки.
// Нельзя выгружать DLL/снимать перехваты, пока жив расширенный TeamInfo.
bool InstallTeamCapacity(bool dedicatedServer);
bool TeamCapacityInstalled();
// Проверяет реальные обработчики на изолированной памяти с защитными словами.
bool TestTeamCapacityMemory();
}
