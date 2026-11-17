// ModdingInterface.h — контракт «игра <-> мод-система».
//
// Взят из veniceext.dll и ПРОВЕРЕН на настоящей DLL
// (C:\Games\re\vtable_shape.py, разбор статический — сама DLL падает в
// DllMain вне игры):
//   * ровно ОДИН экспорт GetModdingInterface();
//   * возвращает объект 4 байт = одна ссылка на vtable (push 4 перед
//     operator new в его коде);
//   * vtable из РОВНО 5 виртуальных методов, без слота деструктора;
//   * ленивый синглтон, без параметров.
// Наша DLL повторяет контракт, чтобы можно было подставить свою реализацию
// вместо veniceext.dll.
//
// Отличие от оригинала: вызывать нас будет наш загрузчик, а не пропатченная
// игра, поэтому контракт тот же, но первый метод мы вызываем сами при
// загрузке DLL.

#pragma once

#include <Windows.h>

#include <string>

namespace evbots {

/// Идентификаторы компонентов Frostbite, к которым мы обращаемся.
/// Заполняются при инициализации из движка, а не зашиваются.
struct EngineNatives {
    // Базовые таблицы Frostbite (у veniceext они по адресам 0x18E74Cxx..0x18E753xx).
    // Здесь храним найденные указатели на функции движка.
    void* onNewRound = nullptr;   ///< ServerPlayerSquadManager::onNewRound
    void* tick = nullptr;         ///< ServerPlayer::update
    void* chatMessage = nullptr;  ///< ServerChatManager::handleMessage
    void* onDamage = nullptr;     ///< ServerSoldierHealthModuleImpl::onDamage

    bool Any() const {
        return onNewRound || tick || chatMessage || onDamage;
    }
};

/// Состояние, которое мод-система возвращает по запросу игры.
struct EngineState {
    unsigned long tickCount = 0;
    unsigned long roundCount = 0;
    bool levelLoaded = false;
    char levelName[128] = {};
};

/// Объект мод-системы. Ровно 5 виртуальных методов, как у veniceext.
///
/// ВИРТУАЛЬНОГО ДЕСТРУКТОРА ЗДЕСЬ НЕТ — и это не упущение, а требование
/// контракта. Проверено на настоящей veniceext.dll (C:\Games\re\vtable_shape.py):
///
///   объект   ровно 4 байта (push 4 перед operator new);
///   vtable   RVA 0x077BD830 в .rdata;
///   слотов   ровно 5 — [0]..[4], дальше уже данные ("1.16" как ASCII);
///   слот [0] НЕ деструктор: у него пролог с установкой SEH-рамки
///           (push ebp / mov eax, fs:[0] / push -1) — то есть это обычный
///           метод инициализации.
///
/// Если добавить виртуальный деструктор, MSVC поставит его отдельным
/// слотом 0 и сдвинет все пять методов на единицу — контракт сломается.
class ModdingInterface {
public:
    /// [0] Инициализация: заполнить таблицы нативов движка.
    /// Возвращает true, если движок найден и таблицы собраны.
    virtual bool Initialize() = 0;

    /// [1] Тик: вызывается каждый игровой тик.
    virtual void OnTick() = 0;

    /// [2] Новая игра / раунд: сбросить состояние.
    virtual void OnNewRound() = 0;

    /// [3] Потокобезопасный доступ к состоянию (для модов и RCON).
    virtual void QueryState(EngineState* out) const = 0;

    /// [4] Остановка: снять хуки и освободить ресурсы.
    virtual void Shutdown() = 0;
};

/// Наш экземпляр. Создаётся лениво, как в оригинале.
ModdingInterface* GetInstance();

// Only from the native resource callback after its DataContainer/TLS scope is
// established. Delivers deferred server Engine:Init / Extension:Loaded once.
bool EnsureNativeModStartup();

// ── клиентский режим (сборка 215, К3) ─────────────────────────────────────
// В bf3.exe (клиент) серверный Initialize не вызывается: хуков сервера нет. Поднимается только клиентский Lua
// (LuaContext::Client): ext\shared + ext\client модов из modsRoot, затем Engine:Init и Extension:Loaded, как у VU.
// Пятиметодный контракт не меняется — это свободные функции.

/// Запустить клиентский Lua с модами из modsRoot (пусто — без модов). true — Lua поднялся.
bool StartClientMode(const std::wstring& modsRoot);
/// Extension:Unloading и закрыть клиентский Lua (если поднимался).
void StopClientMode();
/// Клиентский режим запущен.
bool ClientModeActive();
/// ★ Сборка 228: «кадр клиента» без движка — для проверки WebUI и ввода без игры (EVBots.TestApi, EVBOTS_WEBUI_TEST=1):
/// NetEvents, вызовы страниц WebUI, Engine:Update, ввод и Client:UpdateInput — в том же порядке, что хук кадра.
void ClientFrameForTest(float dt);

}  // namespace evbots

/// Единственный экспорт DLL — как у veniceext.dll.
extern "C" __declspec(dllexport) void* __cdecl GetModdingInterface();
