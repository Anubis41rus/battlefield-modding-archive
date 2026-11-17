// BlazeSession.cs — одно соединение клиента с бэкендом.

using System.Net;
using System.Security.Cryptography.X509Certificates;
using EVBots.Backend.Tdf;

namespace EVBots.Backend.Blaze;

internal sealed class BlazeSession(
    BlazeServices services,
    bool isRedirector,
    IPAddress address,
    Log log) : IDisposable
{
    private static long _next;
    private readonly Lock _writeGate = new();

    public long Id { get; } = System.Threading.Interlocked.Increment(ref _next);
    public BlazeServices Services { get; } = services;
    public Log Log { get; } = log;
    public IPAddress Address { get; } = address;
    public bool IsRedirector { get; } = isRedirector;

    /// <summary>Кем представился клиент (personaref). 0 = неизвестно.</summary>
    public ulong Personaref { get; set; }

    /// <summary>Имя солдата.</summary>
    public string SoldierName { get; set; } = "";

    /// <summary>Почта аккаунта (локальная, вида Имя@evbots.local).</summary>
    public string Mail { get; set; } = "";

    /// <summary>Имя службы из preAuth: CDAT.SVCN.</summary>
    public string Service { get; set; } = "";

    /// <summary>Тип клиента из preAuth: CDAT.TYPE.</summary>
    public int ClientType { get; set; }

    /// <summary>Локализация из preAuth: CINF.LOC.</summary>
    public ulong Localization { get; set; }

    /// <summary>Внутренний адрес клиента (из updateNetworkInfo).</summary>
    public ulong InternalIp { get; set; }

    public int InternalPort { get; set; }

    /// <summary>Аутентифицирован ли клиент.</summary>
    public bool Authenticated { get; set; }

    /// <summary>Игра, в которой клиент состоит.</summary>
    public ulong GameId { get; set; }

    /// <summary>Куда слать уведомления (заполняется при входе в игру).</summary>
    public BlazeSession? GameHost { get; set; }

    /// <summary>
    /// Отложенные join-уведомления: время, после которого (на первом же
    /// пакете клиента) отправить PlayerJoining/Claiming/InitiateConnections.
    /// Рабочий бэкенд шлёт их только когда клиент догрузил карту (~23 с);
    /// мгновенная отправка во время загрузки — клиент mesh не поднимает.
    /// </summary>
    public DateTime? JoinNotifyDue { get; set; }

    /// <summary>Слот игрока, сохранённый при joinGame для отложенной отправки.</summary>
    public int PendingJoinSlot { get; set; }

    public long LastSeenTicks { get; private set; } =
        DateTime.UtcNow.Ticks;

    public TimeSpan Idle => DateTime.UtcNow - new DateTime(LastSeenTicks);

    public void Touch() => LastSeenTicks = DateTime.UtcNow.Ticks;

    private SslStreamHolder? _stream;

    public void AttachStream(SslStreamHolder stream) => _stream = stream;

    public void Reply(ushort component, ushort command, int messageId,
        IEnumerable<TdfNode> payload, int errorCode = 0)
    {
        var type = errorCode > 0
            ? BlazeMessageType.ErrorReply
            : BlazeMessageType.Reply;
        Send(component, command, (ushort)errorCode, type, messageId,
            TdfCodec.Encode(payload));
    }

    public void ReplyError(ushort component, ushort command, int messageId,
        int errorCode)
    {
        Send(component, command, (ushort)errorCode, BlazeMessageType.ErrorReply,
            messageId, []);
    }

    public void Notify(ushort component, ushort command, IEnumerable<TdfNode> payload) =>
        Send(component, command, 0, BlazeMessageType.Notification, 0,
            TdfCodec.Encode(payload));

    public void SendRaw(ushort component, ushort command, int messageId,
        byte[] payload)
    {
        // сырой ответ на запрос; messageId обязателен — клиент связывает
        // реплай с запросом по нему (иначе фаза getStats не запускается)
        Send(component, command, 0, BlazeMessageType.Reply, messageId, payload);
    }

    /// <summary>Был ли уже отправлен ответ на текущий запрос.</summary>
    public bool Replied { get; private set; }

    public void ResetReplyFlag() => Replied = false;

    private void Send(ushort component, ushort command, ushort error,
        BlazeMessageType type, int messageId, byte[] payload)
    {
        // ответ на запрос помечает флаг; уведомления — нет
        if (type is BlazeMessageType.Reply or BlazeMessageType.ErrorReply)
        {
            Replied = true;
        }

        var frame = BlazeFrame.Build(component, command, error, type, messageId,
            payload);

        Log.Debug($"отправка {Convert.ToHexString(frame)}");

        var target = _stream;
        if (target is null)
        {
            Log.Debug($"нет потока для сессии {Id}, кадр отброшен");
            return;
        }

        try
        {
            // Диагностика 13.10: если запись блокируется, в логе останется
            // «send begin» без «send end» — это место зависания.
            Log.Info($"[send begin] сессия={Id} comp=0x{component:X} cmd=0x{command:X} тип={type} id={messageId}");
            lock (_writeGate)
            {
                target.Write(frame);
            }
            Log.Info($"[send end] сессия={Id} cmd=0x{command:X}");
        }
        catch (Exception ex)
        {
            Log.Info($"сессия {Id}: отправка не удалась: {ex.Message}");
        }
    }

    public override string ToString() =>
        $"#{Id}{(IsRedirector ? " redir" : "")} " +
        $"{Address}{(Personaref > 0 ? $" p={Personaref}" : "")}" +
        $"{(Authenticated ? " auth" : "")}";

    public void Dispose()
    {
        _stream?.Dispose();
        _stream = null;
    }
}

/// <summary>Обёртка над TLS-потоком для записи кадров.</summary>
internal sealed class SslStreamHolder(Stream stream) : IDisposable
{
    private readonly Lock _gate = new();
    private readonly Stream _stream = stream;

    public void Write(byte[] data)
    {
        lock (_gate)
        {
            _stream.Write(data, 0, data.Length);
            _stream.Flush();
        }
    }

    public void Dispose() => _stream.Dispose();
}