// LuaRuntime.h — Lua 5.4.2 с окружением VeniceEXT: загрузка модов из
// Admin\Mods, свой require по правилам VU, json и string.starts/ends.
//
// Правила взяты из документации VU (content\modding\structure.md,
// content\vext\guides\code-splitting.md), не придуманы:
//   - запускается только __init__.lua из ext\{client,server,shared};
//   - require(path) ищет относительно ext\<контекст>, .lua необязателен;
//   - shared-файлы только с префиксом __shared/;
//   - путь всегда от корня ext\<контекст>, а не от папки скрипта;
//   - скрипт выполняется один раз, return возвращается в require.

#pragma once

#include <mutex>
#include <string>
#include <vector>

struct lua_State;

namespace evbots {

enum class LuaContext {
    Server,  ///< выделенный сервер (bf3_server_Final.exe)
    Client,  ///< клиент игры
};

struct LoadedMod {
    std::wstring folder;
    std::string name;
    std::string version;
    bool hasVeniceExt = false;
    std::string dependsVeniceExt;
    bool ranInit = false;
    std::string error;
};

class LuaRuntime {
public:
    static LuaRuntime& Instance();

    /// Создать состояние Lua и окружение VEXT. modsRoot — папка Admin\Mods.
    bool Initialize(const std::wstring& modsRoot, LuaContext context);

    /// Сначала полностью проверить набор модов и зависимости, без запуска Lua.
    /// EVBOTS_ACTIVE_MODS: имена папок через ';', @none — пустой набор;
    /// переменная отсутствует — все моды. Возвращает число выбранных или -1
    /// с lastError(); после ошибки runInitScripts не исполняет никакие init.
    int LoadMods();

    void Shutdown();

    lua_State* state() const { return state_; }
    LuaContext context() const { return context_; }
    const std::vector<LoadedMod>& mods() const { return mods_; }
    const std::string& lastError() const { return lastError_; }
    bool ready() const { return state_ != nullptr; }

    /// Однократный запуск __init__.lua всех модов (используется и на сервере,
    /// и у клиента — у клиента свой экземпляр состояния).
    int runInitScripts();

    // ── движковые события → Lua ───────────────────────────────────────────
    // В VU их рассылает veniceext.dll по хукам движка; здесь — та же точка,
    // но вызывается из нашего кода (ModdingInterface / EngineHooks).
    // Имена и сигнатуры — строго по VU-Docs (ref\server\event и ref\shared\event).

    /// Аргумент движкового события: строка, число, булево или nil.
    struct EngineEventArg {
        enum class Kind { None, String, Number, Boolean };
        Kind kind = Kind::None;
        std::string text;
        double number = 0.0;
        bool boolean = false;

        static EngineEventArg Nil() { return EngineEventArg{}; }
        static EngineEventArg Str(std::string s) {
            EngineEventArg a;
            a.kind = Kind::String;
            a.text = std::move(s);
            return a;
        }
        static EngineEventArg Num(double v) {
            EngineEventArg a;
            a.kind = Kind::Number;
            a.number = v;
            return a;
        }
        static EngineEventArg Bool(bool v) {
            EngineEventArg a;
            a.kind = Kind::Boolean;
            a.boolean = v;
            return a;
        }
    };

    /// Расслать движковое событие модам через Events:Dispatch.
    /// Возвращает false, если Lua ещё не готова или обработчик упал.
    bool DispatchEngineEvent(const char* name,
                             const std::vector<EngineEventArg>& args = {});

    /// ★ Сборка 161: вызвать _G[table][fn](args...) под локом Gate (без self).
    /// waitMs < 0 — ждать лок сколько нужно; иначе try_lock не дольше waitMs
    /// (поток загрузки движка не должен вставать в вечное ожидание потока Lua).
    /// Возвращает false, если лок не взят, функции нет или она упала.
    /// Сборка 199: outNum != nullptr — первый результат функции, если он число (иначе не трогается).
    bool CallTableFunction(const char* table, const char* fn,
                           const std::vector<EngineEventArg>& args,
                           int waitMs = -1, double* outNum = nullptr);

    /// Только для теста: добавить в реестр PlayerManager «живого» игрока
    /// (onlineId != 0, чтобы fun-bots не счёл его ботом). Обычные живые
    /// игроки приходят через движковые события, которых в тест-хосте нет.
    bool AddTestHumanPlayer(const std::string& name, int team, int squad);

    /// Корневая папка Admin\Mods (все моды).
    const std::wstring& modsRoot() const { return modsRoot_; }

    /// Папка мода, чей скрипт исполняется прямо сейчас (для require).
    const std::wstring& currentModFolder() const { return currentMod_; }

    /// Вокруг выполнения обработчика события ставим «текущего мода» =
    /// владельца подписки, иначе require внутри обработчика не найдёт файлы
    /// (относительный путь считается от мода). Возвращает прошлое значение.
    std::wstring PushCurrentMod(const std::wstring& folder) {
        const std::wstring prev = currentMod_;
        currentMod_ = folder;
        return prev;
    }

    /// Вернуть предыдущее значение после PushCurrentMod.
    void PopCurrentMod(const std::wstring& prev) { currentMod_ = prev; }

    // Папка модов: переменная окружения EVBOTS_MODS, затем рабочий
    // каталог процесса, затем Instance рядом. Пусто — не нашли.
    static std::wstring DiscoverModsRoot();

    /// Путь относительно ext\context текущего мода — по правилам VU
    /// require всегда от корня ext\context, а не от папки скрипта.
    std::wstring ResolveModPath(const std::wstring& rel) const;

    LoadedMod* findModByFolder(const std::wstring& folder);

    /// Сборка 191: тот же лок, что у всех входов в Lua — для замера ожидания в таймере тика (ModdingInterface.cpp).
    std::recursive_mutex& TickGate() { return Gate(); }

private:
    LuaRuntime() = default;

    /// Единственный вход в Lua из любого потока: lua_State не
    /// потокобезопасен, а у нас минимум два писателя — фоновый таймер
    /// Engine:Update и хуки движка с главного потока. Любой метод, который
    /// трогает state_, обязан взять этот лок (recursive — Initialize зовёт
    /// runInitScripts и т.д.).
    std::recursive_mutex& Gate() {
        static std::recursive_mutex m;
        return m;
    }

    bool createState();
    void applySandbox();
    void registerStringExtensions();
    void registerJson();
    void registerRequire();
    void declareVeniceExt();
    void registerMiddleClass();
    void registerEvents();
    void registerDebugConsole();
    void registerVextEnums();
    void registerVextUtils();
    void registerVextGeometry();
    void registerVextSpatial();
    void registerVextEntityManager();
    void registerVextGlobals();
    void registerVextHooks();
    void registerVextNet();
    void registerVextNetEvents();
    void registerVextPlayerManager();
    void registerVextResourceManager();
    void registerVextTicketManager();
    void registerVextSql();
    void registerVextRcon();
    void registerVextTeamSquadManager();
    void registerVextChatManager();
    void registerVextCustomization();
    void registerVextConfig();

    bool runScriptFile(const std::wstring& path, const std::string& chunkName,
                       std::string* error);

    lua_State* state_ = nullptr;
    std::wstring modsRoot_;
    LuaContext context_ = LuaContext::Server;
    std::vector<LoadedMod> mods_;
    std::string lastError_;
    std::wstring currentMod_;  ///< мод, чей скрипт сейчас исполняется (require)
    bool initRan_ = false;
    bool selectionFailed_ = false;
};

}  // namespace evbots
