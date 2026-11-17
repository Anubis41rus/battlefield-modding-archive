// EvFrame.cs — кадр EV-канала (backend\EV-PROTOCOL.md §2.3).
//
// EV — наш протокол: смысл сообщений как у Zeus (module_id/type_id из ZEUS-MESSAGES.md проекта EV пользователя),
// но формат свой и простой — LAN, без DH/AES (у Zeus шифрование нужно для интернета):
//   u32 LE длина (всё после этого поля) | u16 LE module_id | u16 LE type_id | u32 LE request_id | тело JSON UTF-8.
// Ответ несёт тот же request_id; уведомления сервера — request_id = 0.

using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace EVBots.Backend.Ev;

internal readonly record struct EvFrame(ushort Module, ushort Type, uint RequestId, JsonObject Body)
{
    public const int HeaderSize = 8;           // module + type + request_id
    public const int MaxPayload = 1 << 20;     // 1 МБ: моды идут по HTTP, не здесь

    // тело — UTF-8 как есть (кириллица не экранируется \uXXXX)
    private static readonly JsonSerializerOptions JsonOut = new()
    {
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.Create(System.Text.Unicode.UnicodeRanges.All),
    };

    public byte[] Encode()
    {
        var json = Encoding.UTF8.GetBytes(Body.ToJsonString(JsonOut));
        var buf = new byte[4 + HeaderSize + json.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(buf, (uint)(HeaderSize + json.Length));
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(4), Module);
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(6), Type);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(8), RequestId);
        json.CopyTo(buf, 12);
        return buf;
    }

    /// <summary>Разбор payload (без поля длины). Пустое тело = {}.</summary>
    public static EvFrame Decode(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < HeaderSize)
        {
            throw new InvalidDataException($"EV: кадр короче заголовка ({payload.Length} б)");
        }

        var module = BinaryPrimitives.ReadUInt16LittleEndian(payload);
        var type = BinaryPrimitives.ReadUInt16LittleEndian(payload[2..]);
        var req = BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]);
        var body = payload.Length == HeaderSize
            ? new JsonObject()
            : JsonNode.Parse(payload[HeaderSize..]) as JsonObject
              ?? throw new InvalidDataException("EV: тело не JSON-объект");
        return new EvFrame(module, type, req, body);
    }

    /// <summary>Читает один кадр из потока; null — соединение закрыто.</summary>
    public static async Task<EvFrame?> ReadAsync(Stream s, CancellationToken ct)
    {
        var len = new byte[4];
        if (!await ReadExactAsync(s, len, ct))
        {
            return null;
        }

        var n = BinaryPrimitives.ReadUInt32LittleEndian(len);
        if (n < HeaderSize || n > HeaderSize + MaxPayload)
        {
            throw new InvalidDataException($"EV: неверная длина кадра {n}");
        }

        var payload = new byte[n];
        if (!await ReadExactAsync(s, payload, ct))
        {
            return null;
        }

        return Decode(payload);
    }

    private static async Task<bool> ReadExactAsync(Stream s, byte[] buf, CancellationToken ct)
    {
        var got = 0;
        while (got < buf.Length)
        {
            var r = await s.ReadAsync(buf.AsMemory(got), ct);
            if (r == 0)
            {
                return false;
            }

            got += r;
        }

        return true;
    }

    public override string ToString() =>
        $"{EvIds.Name(Module, Type)} (0x{Module:X4}/0x{Type:X4}, req {RequestId}) {Body.ToJsonString(new JsonSerializerOptions { WriteIndented = false })}";
}

/// <summary>ID модулей и сообщений — как у Zeus (ZEUS-MESSAGES.md, сгенерировано из vu-core.dll); только используемые.</summary>
internal static class EvIds
{
    public const ushort Utility = 0xC57E;
    public const ushort Manager = 0xFC57;
    public const ushort Authentication = 0x6B7F;
    public const ushort Server = 0x4B69;
    public const ushort Player = 0x81BF;

    // Utility
    public const ushort InitSession = 0x9B74;
    public const ushort Ping = 0x8874;
    public const ushort OnScreenMessageNotification = 0x842A;

    // Manager
    public const ushort RegisterSession = 0x8084;
    public const ushort ResumeSession = 0x22B2;

    // Authentication
    public const ushort Login = 0x4EB9;
    public const ushort TokenLogin = 0x7439;
    public const ushort Logout = 0x4330;
    public const ushort CreateAccount = 0x6A63;

    // Server (0x4B69)
    public const ushort ServerLogin = 0x47D1;
    public const ushort FetchServers = 0x4FFE;
    public const ushort GetServerInfo = 0x56D0;
    public const ushort JoinServer = 0xD61E;
    public const ushort LeaveServer = 0xF7A7;
    public const ushort AllowPlayer = 0x5EE9;
    public const ushort DenyPlayer = 0x2383;
    public const ushort JoinAllowedNotification = 0x26E1;
    public const ushort JoinDeniedNotification = 0x39ED;
    public const ushort PlayerJoiningNotification = 0xD054;
    public const ushort PlayerLeftNotification = 0x90A4;
    public const ushort RemovedFromServerNotification = 0xCF89;

    /// <summary>Служебный тип ошибки EV (у Zeus ошибки внутри ответов; у нас — общий ответ на неизвестное).</summary>
    public const ushort Error = 0xFFFF;

    public static string Name(ushort module, ushort type) => (module, type) switch
    {
        (Utility, InitSession) => "Utility.InitSession",
        (Utility, Ping) => "Utility.Ping",
        (Utility, OnScreenMessageNotification) => "Utility.OnScreenMessageNotification",
        (Manager, RegisterSession) => "Manager.RegisterSession",
        (Manager, ResumeSession) => "Manager.ResumeSession",
        (Authentication, Login) => "Authentication.Login",
        (Authentication, TokenLogin) => "Authentication.TokenLogin",
        (Authentication, Logout) => "Authentication.Logout",
        (Authentication, CreateAccount) => "Authentication.CreateAccount",
        (Server, ServerLogin) => "Server.ServerLogin",
        (Server, FetchServers) => "Server.FetchServers",
        (Server, GetServerInfo) => "Server.GetServerInfo",
        (Server, JoinServer) => "Server.JoinServer",
        (Server, LeaveServer) => "Server.LeaveServer",
        (Server, AllowPlayer) => "Server.AllowPlayer",
        (Server, DenyPlayer) => "Server.DenyPlayer",
        (Server, JoinAllowedNotification) => "Server.JoinAllowedNotification",
        (Server, JoinDeniedNotification) => "Server.JoinDeniedNotification",
        (Server, PlayerJoiningNotification) => "Server.PlayerJoiningNotification",
        (Server, PlayerLeftNotification) => "Server.PlayerLeftNotification",
        (Server, RemovedFromServerNotification) => "Server.RemovedFromServerNotification",
        (_, Error) => "Error",
        _ => "?",
    };
}
