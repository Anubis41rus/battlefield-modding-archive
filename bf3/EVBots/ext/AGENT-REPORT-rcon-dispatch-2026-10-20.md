# Разбор входящей RCON-команды в bf3_server_Final.exe (20.10.2026, только чтение)

Метод: pefile + capstone по файлу `C:\Games\BF-Portable\Battlefield_3_Server\bf3_server_Final.exe` (x86, база 0x400000, без ASLR).
Скрипты — `C:\Temp\claude\C--Games\f2a78e11-bb1c-4a22-b637-b2491c8d0e57\scratchpad\rcon-re\` (lib.py, b.py, c.py, st.py, fs.py).
Метки: **[Ф]** — факт (адрес + дизассемблер), **[Г]** — гипотеза.

## 1. Главное

- **[Ф] Диспетчер входящих RCON-команд = `0x00DB7F40`** (метод базового класса `fb::ServerRemoteAdministration`, `BFServerAdministration`
  наследует). `thiscall`, **`ret 8`**, аргументы `(Packet* pkt, int flag)`. Строка `"UnknownCommand"` (VA `0x025B5920`) используется
  **только** в нём — `push 0x25b5920` в `0x00DB7FFF`.
- **[Ф] Через него идут все команды**: сетевой RCON, `Startup.txt`, командная строка/ServerOptions.cfg, PunkBuster-exec. Единая точка.
- **[Ф] Рекомендованная точка перехвата — пролог `0x00DB7F40`**, перезапись первых **13 байт** (позиционно-независимые, нет rel32):
  `83 EC 10 53 55 56 8B 74 24 20 57 8B F9`, продолжение в оригинале с `0x00DB7F4D` (`68 40 59 5B 02`).
- **[Ф] Ответ клиенту**: собрать список слов и вызвать `0x00DB7570(this, pkt, wordsObj)` (`ret 8`) — он создаёт сообщение-ответ и
  кладёт его в очередь сообщений. Для одного слова есть готовый `0x00DB7EC0(this, pkt, string*)` (`ret 8`).

## 2. Строки и их ссылки [Ф]

| Строка | VA строки | Кто использует |
|---|---|---|
| `UnknownCommand` | 0x025B5920 | `0x00DB7FFF` (диспетчер) — единственное место |
| `LogInRequired` | 0x025B5910 | диспетчер `0x00DB8056` |
| `CommandRestricted` | 0x025B58FC | диспетчер `0x00DB807F` |
| `CommandDisallowedOnRanked` | 0x025B58E0 | диспетчер `0x00DB80C4` |
| `EmptyCommand` | 0x025B5930 (конец 0x025B593C) | диспетчер `0x00DB7F6E` (0 слов) |
| `InvalidArguments` | 0x025AF670 | десятки обработчиков команд |
| `OK` | 0x025B5AF4 | обработчики (первое слово успешного ответа) |
| `serverInfo` | 0x0264AAD8 (короткое имя `sein` 0x0264AAE4) | регистрация `0x0114C672` |
| `admin.say` / `admin.killPlayer` / `mapList.list` | 0x0264AA24 / 0x0264A9F4 / 0x0264A7A4 | регистрации `0x0114CF21` / `0x0114D125` / `0x0114E5EE` |

Регистрации всех команд — в функциях класса `BFServerAdministration` (напр. `0x0114C4D0` регистрирует `serverInfo`, обработчик `0x01146940`).

## 3. Путь входящего пакета [Ф]

```
TCP -> ServerRemoteAdministrationConnection::update  0x00DB3010 (thiscall, ecx=conn; размер conn 0x10080)
     читает заголовок в conn+0x74: [0]=флаги/размер (bit31, bit30=ответ), [4]=полный размер, [8]=число слов,
     далее слова: u32 длина + символы + NUL.  Слова копируются в локальный Packet (ctor 0x00DAB000 по 0x00DB30FF,
     addWord 0x00DB2C80 по 0x00DB3148), [pkt+0x28]=conn (mov [esp+0x3c],esi).
     bit30==0 (запрос) -> call 0x00549A20 (ecx=[0x02B3EE2C] — менеджер сообщений; push &pkt)  по 0x00DB316D
     bit30==1 ("Event Response")  -> только лог
менеджер сообщений -> BFServerAdministration::onMessage = 0x01152A00 (наш слот vtable 0x02649BF0; ret 4)
     0x01152A17: call 0x00DBB380(msg)      (ecx=this)
0x00DBB380  (ret 4): если msg[+4]==0xF9C35B8E && msg[+8]==0x76937D58 (ServerAdministrationPacketMessageBase),
     msg[+0x2D]==0 (не ответ) и msg[+0x28]!=0 (conn):   push 0 ; push msg ; mov ecx,edi ; call 0x00DB7F40   (0x00DBB3C8)
```
Следствие: диспетчер вызывается из потока обработки сообщений (тот же, где работает наш перехват `onMessage`) — **[Г]** значит
Lua-обработчик можно вызывать прямо из хука без отдельной синхронизации (подтвердить прогоном).

Другие вызывающие диспетчера (все с локальным Packet): `0x00DB84F2` (ServerOptions/командная строка, "Processing rcon commands…"),
`0x00DB8703` (Startup.txt), `0x00DBA0A5` и `0x00DBA1F6` ("Executing remote admin command:"), `0x00DBA388`, `0x00DBB338`,
`0x0114370B`, `0x01152F51` (`PunkBusterExecCmdMessage` внутри `onMessage`). Глобальный `RemoteAdministration` — `[0x02B4357C]`
(пишется ctor `0x00DB2DFF`, обнуляется в `0x00DAF5BC`).

## 4. Диспетчер 0x00DB7F40 — листинг ключевых мест [Ф]

```
00db7f40  83ec10        sub esp,0x10                 ; 16 байт локальная строка-ответ [esp+0x10..] после pushes
00db7f43  53 / 55 / 56  push ebx/ebp/esi
00db7f46  8b742420      mov esi,[esp+0x20]           ; esi = Packet* pkt   (arg1)   [arg2 = int flag, не читается до вызова обработчика]
00db7f4a  57            push edi
00db7f4b  8bf9          mov edi,ecx                  ; edi = this (ServerRemoteAdministration/BFServerAdministration)
00db7f4d  6840595b02    push 0x25b5940 ; call 0xda2200   ; лог "Command Request"
00db7f59  mov eax,[esi+0x3fcc] ; sub eax,[esi+0x3fc8] ; test eax,0xfffffffc ; jne -> не пусто
          ; слов нет -> строка "EmptyCommand" (0x25b5930..0x25b593c) -> ответ 0xdb7ec0
00db7fce  mov edx,[esi+0x3fc8] ; mov eax,[edx] ; add eax,[esi+0x38]   ; eax = char* слово[0]
00db7fe8  call 0x402090                              ; строка из C-строки (имя команды)
00db7fed  mov ecx,edi ; call 0xda2940                ; поиск команды по имени (ret 0x10, строка по значению) -> ebp = entry*, 0 если нет
00db7ff6  cmp ebp,ebx ; jne 0xdb8016
00db7ffa  lea eax,[esp+0x28] ; push eax ; push 0x25b5920 ; lea ecx,[esp+0x18] ; call 0x402090   ; строка "UnknownCommand"
00db8011  jmp 0xdb7f91 -> lea ecx,[esp+0x10]; push ecx; push esi; mov ecx,edi; call 0xdb7ec0   ; отправка одного слова-ответа
00db8016  mov eax,[esi+0x28]                         ; conn
00db8046  cmp byte [ebp+0x2c],bl ; je 0xdb8072       ; entry.requiresLogin
00db804b  test byte [eax+0x6c],1 ; jne 0xdb8072      ; conn.loggedIn  (иначе "LogInRequired")
00db8072  mov ecx,[ebp+0x28] ; cmp ecx,[eax+0x68] ; jge 0xdb809b   ; иначе "CommandRestricted"
00db80ab..  "CommandDisallowedOnRanked" при ranked-флаге ([0x02877500]+0x94) и entry.flags&4
00db8130  mov ecx,[esp+0x28] (bool) ; mov edx,[ebp+4] ; push ecx ; mov ecx,[ebp] ; push esi ; call edx   ; ОБРАБОТЧИК
00db8145  ret 8
```
Entry команды (создаётся `0x00DA20C0`, `ret 0x14`) **[Ф]**: `[+0]` объект-this обработчика, `[+4]` указатель функции, `[+8..]` строка (описание),
`[+0x28]` int (уровень доступа, сравнивается с `conn+0x68`), `[+0x2C]` byte (требует входа), `[+0x30]` флаги (бит 1/2/4 — смысл **[Г]**).
Обработчик: `thiscall(obj, Packet* pkt, uint bool)`, **`ret 8`**, сам формирует и отправляет ответ.

Таблицы команд: длинное имя — map в `this+0xA8`, короткое (4 буквы) — в `this+0x88` (`0x00DA2940`).
Регистрация (используется `0x0114C4D0` и др.): `entry = 0x00DA20C0(tmp, &delegate{obj,fn}, 0/*byte requiresLogin*/, 1/*level*/, &descString, 0/*flags*/)`
затем **`0x00DB2E60(this, &longName, &shortName(ровно 4 символа!), entry)`**, `ret 0xC`. Сверено по `0x0114C68A–0x0114C6BF`.

## 5. Слова пакета и формирование ответа [Ф]

Packet (vtable 0x025B3F68, ctor `0x00DAB000` thiscall ecx=буфер, надо ≥ ~0x6A40 байт; базовый ctor `0x02320760`, деструктор — `0x005306A0`):

| Поле | Смысл |
|---|---|
| `+0x28` | `ServerRemoteAdministrationConnection*` (логин: `conn+0x6C & 1`, уровень `conn+0x68`) |
| `+0x2C` byte | бит31 заголовка (источник), `+0x2D` byte — "это ответ", `+0x30` — номер последовательности (копируется в ответ) |
| `+0x38` | begin символьного буфера (`=pkt+0x48`), `+0x3C` end, `+0x40` cap |
| `+0x3FC8/+0x3FCC` | begin/end вектора **смещений слов** (u32 на слово) |

**Слово i** = `(const char*)( *(char**)(pkt+0x38) + ((uint32_t*)*(void**)(pkt+0x3FC8))[i] )` (NUL-terminated; перед ним u32 длина). **Число слов** =
`(*(uint32_t*)(pkt+0x3FCC) - *(uint32_t*)(pkt+0x3FC8)) >> 2` (включая имя команды в слове 0). Это не eastl::string-вектор, а плоский буфер.

`addWord = 0x00DB2C80`: `thiscall`, **ecx = Packet+0x38**, `push const char*`, **`ret 4`** — дописывает u32 длину + строку + NUL и смещение в вектор.
`sendWords = 0x00DB7570`: `thiscall(this)`, `push wordsObj` (= Packet+0x38 собираемого пакета), `push origPkt`, **`ret 8`**:
создаёт ответ (`vtable 0x25B3F68`; копирует conn `[+0x28]`, `[+0x2C]`, `[+0x30]`, ставит `[+0x2D]=1`, копирует слова) и `call 0x00549A20` ([0x02B3EE2C]) — отправка через очередь сообщений.
Типичный обработчик (`serverInfo` `0x01146940`): локальный Packet-подобный объект, `addWord(0x25B5AF4 "OK")`, остальные слова, затем `0x00DB7570`.
Ошибки (`UnknownCommand`, `InvalidArguments`, …) — одно слово через `0x00DB7EC0(this, pkt, string*)`, где `string*[0]` = `char*` begin.

Пример готового локального объекта из `0x00DB7EC0` (альтернатива ctor): `W[+0]=W[+4]=W+0x10; W[+8]=W+0x3F90; W[+0x3F90]=W[+0x3F94]=W+0x3FA0; W[+0x3F9C]=W+0x69F4` — но надёжнее
вызвать ctor `0x00DAB000`.

## 6. Рекомендованный перехват и псевдокод

Точка: `0x00DB7F40`, проверить перед записью 13 байт `83 EC 10 53 55 56 8B 74 24 20 57 8B F9` (иначе не ставить). Трамплин = эти 13 байт + `jmp 0x00DB7F4D`.
(Если нужно «только когда не найдено» — альтернатива: патч `0x00DB7FF8 (75 1C jne)`, менее удобно.)

```cpp
// __fastcall(ecx=this, edx=мусор, 2 стековых арг) == thiscall + ret 8
static void __fastcall HookDispatch(void* self, void*, uint8_t* pkt, int flag) {
    uint32_t* b = *(uint32_t**)(pkt + 0x3FC8), *e = *(uint32_t**)(pkt + 0x3FCC);
    int n = int(e - b);                      // число слов
    if (n > 0) {
        const char* base = *(const char**)(pkt + 0x38);
        const char* name = base + b[0];
        if (ModRconHas(name)) {                       // реестр команд мода (VextRcon)
            uint8_t* conn = *(uint8_t**)(pkt + 0x28);
            bool loggedIn = conn && (conn[0x6C] & 1);
            // права: если RemoteCommandFlag требует входа и !loggedIn -> ответ "LogInRequired"
            std::vector<std::string> args;            // слова 1..n-1
            for (int i = 1; i < n; ++i) args.push_back(base + b[i]);
            std::vector<std::string> res = ModRconRun(name, args, loggedIn);   // Lua callback(command,args,loggedIn) -> string{}
            ReplyWords(self, pkt, {"OK", res...});    // ниже
            return;                                   // движку не отдаём (ret 8 делает __fastcall)
        }
    }
    g_origDispatch(self, pkt, flag);                  // трамплин: thiscall ret 8
}

static void ReplyWords(void* self, void* pkt, const std::vector<const char*>& w) {
    alignas(16) static uint8_t buf[0x6B00];           // или куча; вызов из одного потока
    typedef void (__thiscall *Ctor)(void*);       ((Ctor)0x00DAB000)(buf);
    typedef void (__thiscall *Add)(void*, const char*);   // ret 4
    for (auto s : w) ((Add)0x00DB2C80)(buf + 0x38, s);
    typedef void (__thiscall *Send)(void*, void*, void*); // ret 8: (this, origPkt, wordsObj)
    ((Send)0x00DB7570)(self, pkt, buf + 0x38);
    typedef void (__thiscall *Dtor)(void*);       ((Dtor)0x005306A0)(buf);
}
```
Ответ об ошибке одного слова: `0x00DB7EC0(self, pkt, &str)` либо `ReplyWords(..., {"InvalidArguments"})`. Вызов обработчиков движка не нужен.

## 7. Альтернатива без патча кода (**[Г]**, механизм сверен, но не проверен запуском)
Регистрировать команды мода в таблице самого движка: `entry = 0x00DA20C0(tmp, &delegate{obj=свой, fn=свой_thiscall_ret8}, requiresLogin, level, &descString, flags)`,
затем `0x00DB2E60(g_admin /*[0x02B4357C]*/, &longName, &shortName4, entry)`. Тогда движковый диспетчер сам даёт `LogInRequired/CommandRestricted`, а наш `fn(obj, pkt, flag)` ret 8 читает слова
и отвечает `ReplyWords`. Плюсы: нет перезаписи кода, «help»-подобное и права как у движка. Минусы: нужно имя ровно из 4 символов для короткого (проверить, можно ли передать пустое/уникальное),
строки `0x00DA20C0`/`0x00DB2E60` требуют валидных 16-байтных строк ({begin,end,cap,alloc} через `0x004025C2(begin,end)` — как в `0x0114C66C`), и вызов до загрузки `BFServerAdministration::registerCommands`.

## 8. Что не проверено
- Запуском на стенде ничего не гонялось (по заданию только чтение).
- Потоки: считается, что `onMessage` и диспетчер идут в одном потоке обработки сообщений — подтвердить.
- Точный смысл `entry+0x30` флагов и `conn+0x68` (уровни) — по коду видно сравнение, значения не расшифрованы.
- Регистр имён: сравнение в `0x00DA2940` может быть регистронезависимым — не разбирал; в хуке сравнивать как в VU/мод (без регистра безопаснее проверить на стенде).
