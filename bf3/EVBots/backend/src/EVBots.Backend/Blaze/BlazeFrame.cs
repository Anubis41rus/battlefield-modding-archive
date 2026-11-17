// BlazeFrame.cs — кадр Blaze: заголовок, типы сообщений, разбор и сборка.

using EVBots.Backend.Tdf;

namespace EVBots.Backend.Blaze;

/// <summary>Тип сообщения Blaze (старшие 4 бита байта 8).</summary>
internal enum BlazeMessageType : byte
{
    Request = 0,
    Reply = 1,
    Notification = 2,
    ErrorReply = 3,
}

/// <summary>Разобранный заголовок кадра.</summary>
internal readonly record struct BlazeFrameHeader
{
    public int PayloadLength { get; init; }
    public ushort ComponentId { get; init; }
    public ushort CommandId { get; init; }
    public ushort ErrorCode { get; init; }
    public BlazeMessageType MessageType { get; init; }
    public int MessageId { get; init; }
}

internal static class BlazeFrame
{
    public const int HeaderSize = 12;

    /// <summary>
/// Разбор заголовка кадра.
///
/// Заголовок — ровно <see cref="HeaderSize"/> байт. Если бит 4 байта 9
/// установлен, СЛЕДОМ за заголовком идут ещё 2 байта старшей половины
/// размера payload, и только потом сам payload. Эти 2 байта читает вызывающий
/// и передаёт сюда вторым параметром: читать их из заголовка нельзя —
/// индекс 12 за границей массива.
/// </summary>
internal static BlazeFrameHeader ParseHeader(byte[] h, int extendedHigh = -1)
    {
        // 16-битные поля заголовка Blaze — big-endian (проверено по рабочему
        // эмулятору и по живой трассе BF3: Redirector=5, GetServerInstance=1).
        int payload = (h[0] << 8) | h[1];
        ushort component = (ushort)((h[2] << 8) | h[3]);
        ushort command = (ushort)((h[4] << 8) | h[5]);
        ushort error = (ushort)((h[6] << 8) | h[7]);
        var type = (BlazeMessageType)((h[8] >> 4) & 0x0F);

        // extendedHigh — 16-битная старшая половина размера (байты 12..13
        // кадра). Складывать надо в long: в int сдвиг на 16 дал бы
        // отрицательную длину при payload >= 16 МиБ (0x8000:0000).
        if (extendedHigh > 0)
        {
            var full = (long)((uint)payload |
                              ((uint)(extendedHigh & 0xFFFF) << 16));
            payload = full > int.MaxValue ? int.MaxValue : (int)full;
        }

        // message ID: младший байт — h[11], средний — h[10], старший ниббл — h[9]
        int messageId = h[11] | (h[10] << 8) | ((h[9] & 0x0F) << 16);

        return new BlazeFrameHeader
        {
            PayloadLength = payload,
            ComponentId = component,
            CommandId = command,
            ErrorCode = error,
            MessageType = type,
            MessageId = messageId,
        };
    }

    /// <summary>Есть ли в кадре расширенный размер payload.</summary>
    public static bool HasExtendedSize(byte[] header) => (header[9] & 0x10) != 0;

    /// <summary>Размер дополнительных байт после заголовка (0 или 2).</summary>
    public static int ExtendedSizeBytes(byte[] header) =>
        HasExtendedSize(header) ? 2 : 0;

    /// <summary>Собирает кадр с payload'ом.</summary>
    public static byte[] Build(ushort component, ushort command, ushort error,
        BlazeMessageType type, int messageId, byte[]? payload)
    {
        payload ??= [];

        var frame = new byte[HeaderSize + payload.Length];

        // размер: младшие 16 бит, старшие — только если нужно
        var lenLow = payload.Length & 0xFFFF;
        var lenHigh = (payload.Length >> 16) & 0xFFFF;

        // 16-битные поля — big-endian (как в рабочем эмуляторе Blaze)
        frame[0] = (byte)((lenLow >> 8) & 0xFF);
        frame[1] = (byte)(lenLow & 0xFF);

        frame[2] = (byte)(component >> 8);
        frame[3] = (byte)(component & 0xFF);

        frame[4] = (byte)(command >> 8);
        frame[5] = (byte)(command & 0xFF);

        frame[6] = (byte)(error >> 8);
        frame[7] = (byte)(error & 0xFF);

        frame[8] = (byte)(((byte)type & 0x0F) << 4);

        byte b9 = 0;
        if (lenHigh != 0)
        {
            b9 |= 0x10;
        }

        b9 |= (byte)((messageId >> 16) & 0x0F);
        frame[9] = b9;

        // message ID: средний байт — h[10], младший — h[11]
        frame[10] = (byte)((messageId >> 8) & 0xFF);
        frame[11] = (byte)(messageId & 0xFF);

        if (lenHigh != 0)
        {
            // расширенный размер записывается сразу после заголовка
            var ext = new byte[HeaderSize + 2 + payload.Length];
            ext[0] = frame[0];
            ext[1] = frame[1];
            ext[2] = frame[2];
            ext[3] = frame[3];
            ext[4] = frame[4];
            ext[5] = frame[5];
            ext[6] = frame[6];
            ext[7] = frame[7];
            ext[8] = frame[8];
            ext[9] = b9;
            ext[10] = frame[10];
            ext[11] = frame[11];
            ext[12] = (byte)((lenHigh >> 8) & 0xFF);
            ext[13] = (byte)(lenHigh & 0xFF);
            payload.CopyTo(ext, HeaderSize + 2);
            return ext;
        }

        payload.CopyTo(frame, HeaderSize);
        return frame;
    }

    /// <summary>Разбирает payload и передаёт в диспетчер компонентов.</summary>
    public static void Handle(BlazeSession session, BlazeFrameHeader header,
        byte[] payload)
    {
        // сбрасываем флаг «ответ отправлен» на каждый новый запрос
        session.ResetReplyFlag();

        // 18.10: отчёт матча (GameReporting) — свой разбор сырого payload (тип 7 = variable, float BE),
        // общий TdfCodec на нём ошибается. Статистика пишется в progress.json игроков.
        if (header.ComponentId == Component.GameReporting)
        {
            session.Services.HandleGameReport(session, header.CommandId, header.MessageId, payload);
            return;
        }

        TdfFields fields;
        try
        {
            fields = payload.Length > 0 ? TdfCodec.Decode(payload) : new TdfFields();
        }
        catch (Exception ex)
        {
            session.Log.Warn($"TDF не разобран " +
                             $"(comp={header.ComponentId} cmd={header.CommandId}): " +
                             $"{ex.Message}");
            return;
        }

        session.Log.Debug(
            $"запрос comp={header.ComponentId} cmd={header.CommandId} " +
            $"msgId={header.MessageId} полей={fields.Count} " +
            $"[{string.Join(", ", fields.All.Select(f => f.Label))}]");

        session.Services.Dispatch(session, header.ComponentId, header.CommandId,
            header.MessageId, fields);
    }
}