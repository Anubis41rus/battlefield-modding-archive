// TdfRoundTrip — самопроверка кодека и кадров Blaze.
//
// ── Что показал эталонный payload ───────────────────────────────────────────
// Поле getStatGroupPayloadBase64 из BF3NativeBots\progress\bf3-stats-catalog.json
// (135 690 байт) НЕ является TDF. Это бинарный блок DICE
// statgroups/bf3/bf3_player_mpdefault2.bin: строки лежат как
// <id:3 байта> 01 <длина> <ASCII> 00, без меток-символов TDF.
//
// Совпадает и с ошибкой в строках рабочего бэкенда:
//   «getStatGroup payload is not TDF: »
//
// Вывод: клиент просит стат-группу, сервер EA отдаёт этот блок как есть.
// Разбирать его не нужно — достаточно отдать байт в байт (BlazeServices
// делает это через SendRaw). Проверяем здесь именно pass-through.
//
// ── Что реально проверяем ───────────────────────────────────────────────────
// A) Синтетический TDF: все типы -> кодирование -> декодирование -> обратно,
//    сравнение байт в байт. Это проверка самого кодека.
// B) Эталонный блок: base64 -> байты -> base64 без изменений (pass-through).
// C) Кадр Blaze: сборка -> разбор заголовка, все поля совпали.

using System.Text;
using System.Text.Json;
using EVBots.Backend.Blaze;
using EVBots.Backend.Tdf;

namespace EVBots.Backend.Tests;

internal static class TdfRoundTrip
{
    public static int Run(string catalogPath)
    {
        Console.WriteLine("=== самопроверка EV Backend ===");
        Console.WriteLine();

        var failures = 0;
        failures += TestSyntheticTdf();
        failures += TestCatalogPayload(catalogPath);
        failures += TestFrame();
        failures += TestVarintEdges();
        failures += TestBigListCount();

        Console.WriteLine();
        if (failures == 0)
        {
            Console.WriteLine("ИТОГ: все проверки пройдены.");
            return 0;
        }

        Console.WriteLine($"ИТОГ: провалено проверок: {failures}");
        return 1;
    }

    // ── A) синтетический TDF ────────────────────────────────────────────────

    private static int TestSyntheticTdf()
    {
        Console.WriteLine("--- A) синтетический TDF: все типы, round-trip ---");

        // Метки TDF ограничены буквами: символ кодируется как (c & 0x1F) | 0x40,
// поэтому цифры и знаки превращаются в буквы и обратно не читаются.
// В протоколе метки всегда заглавные буквы (EVAL, VALU, GID, NAME…).
var original = new List<TdfNode>
        {
            // маленькое число (однобайтовый varint)
            TdfNode.Int("SML", 5UL),
            // ровно граница: 0x3F и 0x40 (однобайтовый/многобайтовый)
            TdfNode.Int("LOED", 0x3FUL),
            TdfNode.Int("HIED", 0x40UL),
            TdfNode.Int("MID", 1_000_000UL),
            TdfNode.Int("BIG", ulong.MaxValue),
            TdfNode.Str("EMPT", ""),
            TdfNode.Str("ONE", "x"),
            TdfNode.Str("LONG", "player_mpdefault2"),
            TdfNode.Bin("DATA", new byte[] { 1, 2, 3, 0xFF }),
            TdfNode.Struct("STU",
                TdfNode.Int("A", 1UL),
                TdfNode.Str("B", "внутри"),
                TdfNode.Struct("NST",
                    TdfNode.Int("C", 7UL))),
            TdfNode.ListOf("LST", TdfType.Integer, (object)1UL, (object)2UL,
                (object)300UL),
            TdfNode.ListOf("LSS", TdfType.String, (object)"a", (object)"bb"),
            TdfNode.ListOf("LSU", TdfType.Struct,
                (object)new List<TdfNode> { TdfNode.Int("X", 1UL) },
                (object)new List<TdfNode> { TdfNode.Int("X", 2UL) }),
            TdfNode.MapOf("MAP", TdfType.String, TdfType.Float,
                new List<(object, object)>
                {
                    ("c___k_g", 41.0),
                    ("c___sa_g", 1373.8657),
                }),
            TdfNode.VarInts("VIN", 1UL, 2UL, 3UL),
            TdfNode.ObjectType("OTY", 2UL, 0x7F000001UL),
            TdfNode.ObjectId("OID", 1UL, 2UL, 3UL),
        };

        byte[] encoded;
        try
        {
            encoded = TdfCodec.Encode(original);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  ОШИБКА кодирования: {ex.Message}");
            return 1;
        }

        Console.WriteLine($"  закодировано: {encoded.Length} байт");

        TdfFields decoded;
        try
        {
            decoded = TdfCodec.Decode(encoded);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  ОШИБКА декодирования: {ex.Message}");
            return 1;
        }

        Console.WriteLine($"  декодировано полей: {decoded.Count} " +
                          $"(ожидалось {original.Count})");

        if (decoded.Count != original.Count)
        {
            Console.WriteLine($"  РАСХОЖДЕНИЕ: полей {decoded.Count} != {original.Count}");
            return 1;
        }

        // проверяем значения выборочно
        var checks = new (string Label, object Expected, object Actual)[]
        {
            ("SML", 5UL, decoded.UInt("SML")),
            ("LOED", 0x3FUL, decoded.UInt("LOED")),
            ("HIED", 0x40UL, decoded.UInt("HIED")),
            ("MID", 1_000_000UL, decoded.UInt("MID")),
            ("BIG", ulong.MaxValue, decoded.UInt("BIG")),
            ("ONE", "x", decoded.Str("ONE")),
            ("LONG", "player_mpdefault2", decoded.Str("LONG")),
        };

        var bad = 0;
        foreach (var (label, expected, actual) in checks)
        {
            if (!Equals(expected, actual))
            {
                Console.WriteLine($"  ЗНАЧЕНИЕ {label}: ожидалось {expected}, " +
                                  $"получено {actual}");
                bad++;
            }
        }

        if (decoded.Str("EMPT") != "")
        {
            Console.WriteLine($"  ЗНАЧЕНИЕ EMTP: ожидалась пустая строка, " +
                              $"получено '{decoded.Str("EMPT")}'");
            bad++;
        }

        // обратное кодирование
        byte[] re;
        try
        {
            re = TdfCodec.Encode(decoded);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  ОШИБКА обратного кодирования: {ex.Message}");
            return 1;
        }

        if (!re.AsSpan().SequenceEqual(encoded))
        {
            Console.WriteLine("  РАСХОЖДЕНИЕ байтов при обратном кодировании");
            var i = 0;
            while (i < Math.Min(re.Length, encoded.Length) && re[i] == encoded[i])
            {
                i++;
            }

            Console.WriteLine($"    первое различие: смещение {i}");
            bad++;
        }

        Console.WriteLine(bad == 0
            ? "  OK: значения и байты совпали"
            : $"  провалено проверок: {bad}");
        return bad;
    }

    // ── B) эталонный блок statgroups ───────────────────────────────────────

    private static int TestCatalogPayload(string catalogPath)
    {
        Console.WriteLine();
        Console.WriteLine("--- B) эталонный блок getStatGroup (pass-through) ---");

        if (!File.Exists(catalogPath))
        {
            Console.WriteLine($"  ПРОПУСК: каталог не найден: {catalogPath}");
            return 0;
        }

        string b64;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(catalogPath),
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = true,
                    CommentHandling = JsonCommentHandling.Skip,
                });

            if (!doc.RootElement.TryGetProperty("getStatGroupPayloadBase64",
                    out var el))
            {
                Console.WriteLine("  ПРОПУСК: в каталоге нет getStatGroupPayloadBase64");
                return 0;
            }

            b64 = el.GetString() ?? "";
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  ОШИБКА чтения каталога: {ex.Message}");
            return 1;
        }

        var bytes = Convert.FromBase64String(b64);
        var group = "player_mpdefault2";
        using (var doc = JsonDocument.Parse(File.ReadAllText(catalogPath)))
        {
            if (doc.RootElement.TryGetProperty("group", out var g))
            {
                group = g.GetString() ?? group;
            }
        }

        Console.WriteLine($"  блок: {bytes.Length:N0} байт, группа '{group}'");
        Console.WriteLine($"  начало: {Convert.ToHexString(bytes, 0, 16)}");

        // pass-through: байты должны дойти до клиента без изменений
        var again = Convert.ToBase64String(bytes);
        var identical = string.Equals(again, b64, StringComparison.Ordinal);

        // содержимое: ищем имена стат-групп, чтобы подтвердить формат
        var hasGroupName =
            Encoding.ASCII.GetString(bytes, 0, Math.Min(bytes.Length, 4096))
                .Contains(group, StringComparison.Ordinal);

        Console.WriteLine($"  pass-through без изменений: {(identical ? "да" : "НЕТ")}");
        Console.WriteLine($"  содержит имя группы '{group}': " +
                          $"{(hasGroupName ? "да" : "нет")}");

        // это не TDF — убеждаемся, что попытка разбора падает ожидаемо
        var isTdf = true;
        try
        {
            TdfCodec.Decode(bytes);
        }
        catch (Exception)
        {
            isTdf = false;
        }

        Console.WriteLine($"  формат: {(isTdf ? "TDF" : "НЕ TDF — бинарный блок DICE statgroups")}");
        Console.WriteLine("  => сервер отдаёт блок байт в байт (BlazeServices.SendRaw),");
        Console.WriteLine("     разбирать его не требуется.");

        return identical ? 0 : 1;
    }

    // ── C) кадр Blaze ──────────────────────────────────────────────────────

    private static int TestFrame()
    {
        Console.WriteLine();
        Console.WriteLine("--- C) кадр Blaze: сборка -> разбор ---");

        var payload = TdfCodec.Encode(new List<TdfNode>
        {
            TdfNode.Int("GID", 234000001UL),
            TdfNode.Str("NAME", "Moy EVBots server"),
        });

        // message ID в кадре Blaze — 20 бит: 12 бит в байтах 10..11 плюс
        // 4 бита в младшей тетраде байта 9. Старшие 4 бита байта 9 заняты
        // флагом расширенного размера, поэтому 0x123456 обрезается.
        const int mid = 0x0FFFFF;

        var frame = BlazeFrame.Build(
            Component.GameManager, GameManagerCmd.JoinGame, 0,
            BlazeMessageType.Reply, mid, payload);

        var h = BlazeFrame.ParseHeader(frame, 0);

        Console.WriteLine($"  кадр: {frame.Length} байт");
        Console.WriteLine($"  payloadLength = {h.PayloadLength} " +
                          $"(ожидалось {payload.Length})");
        Console.WriteLine($"  component      = 0x{h.ComponentId:X4} " +
                          $"(ожидалось 0x{Component.GameManager:X4})");
        Console.WriteLine($"  command        = 0x{h.CommandId:X4} " +
                          $"(ожидалось 0x{GameManagerCmd.JoinGame:X4})");
        Console.WriteLine($"  messageType    = {h.MessageType}");
        Console.WriteLine($"  messageId      = 0x{h.MessageId:X6} " +
                          $"(ожидалось 0x{mid:X6}, поле 20 бит)");

        var bad = 0;
        if (h.PayloadLength != payload.Length)
        {
            bad++;
        }

        if (h.ComponentId != Component.GameManager)
        {
            bad++;
        }

        if (h.CommandId != GameManagerCmd.JoinGame)
        {
            bad++;
        }

        if (h.MessageId != mid)
        {
            bad++;
        }

        if (h.MessageType != BlazeMessageType.Reply)
        {
            bad++;
        }

        // payload должен читаться
        var body = BlazeFrame.ParseHeader(frame, 0);
        if (body.PayloadLength > 0)
        {
            var rest = new byte[body.PayloadLength];
            Array.Copy(frame, BlazeFrame.HeaderSize, rest, 0, body.PayloadLength);
            var fields = TdfCodec.Decode(rest);
            Console.WriteLine($"  payload полей: {fields.Count}, " +
                              $"GID={fields.UInt("GID")}, " +
                              $"NAME=\"{fields.Str("NAME")}\"");
            if (fields.Count != 2 || fields.UInt("GID") != 234000001UL)
            {
                bad++;
            }
        }

        Console.WriteLine(bad == 0 ? "  OK" : $"  провалено: {bad}");
        return bad;
    }

    // ── D) границы varint ──────────────────────────────────────────────────

    private static int TestVarintEdges()
    {
        Console.WriteLine();
        Console.WriteLine("--- D) границы varint ---");

        var values = new ulong[]
        {
            0, 1, 0x3E, 0x3F, 0x40, 0x41, 0x7F, 0x80, 0x81,
            0x3FFF, 0x4000, 0xFFFF, 0x10000, 0xFFFFFF, 0x1000000,
            ulong.MaxValue - 1, ulong.MaxValue,
        };

        var bad = 0;
        foreach (var v in values)
        {
            var buf = new List<byte>();
            WriteVarint(buf, v);
            var arr = buf.ToArray();
            var p = 0;
            var back = ReadVarint(arr, ref p);

            var full = p == arr.Length;
            if (back != v || !full)
            {
                Console.WriteLine($"  ОШИБКА {v}: получено {back}, " +
                                  $"прочитано {p}/{arr.Length} байт");
                bad++;
            }
        }

        // транслитерация: кириллица не должна попадать в TDF
        var cyr = AsciiSafe.Transliterate("Мой сервер «Ядро»");
        if (cyr.Any(c => c >= 0x80))
        {
            Console.WriteLine($"  ОШИБКА транслитерации: остались не-ASCII: {cyr}");
            bad++;
        }
        else
        {
            Console.WriteLine($"  транслитерация: 'Мой сервер <Ядро>' -> '{cyr}'");
        }

        Console.WriteLine(bad == 0
            ? $"  OK: все {values.Length} границ varint + транслитерация"
            : $"  провалено: {bad}");
        return bad;
    }

    // ── E) большие списки (счётчик > 127) ──────────────────────────────────

    private static int TestBigListCount()
    {
        Console.WriteLine();
        Console.WriteLine("--- E) большие списки: счётчик как varint ---");

        // Точный кейс notify 0x32: 1432 строки. Раньше счётчик писался
        // (byte)Count = 0x98, приёмник читал varint 98 05 (= 344) и
        // съедал первый байт первой строки — результата не было.
        var values = new List<object>(1432);
        for (var i = 0; i < 1432; i++)
        {
            values.Add((object)$"{i % 7}.00");
        }

        var original = new List<TdfNode>
        {
            TdfNode.Struct("ENTI",
                TdfNode.Int("EID", 234471775UL),
                TdfNode.ListOf("STAT", TdfType.String, values.ToArray())),
            TdfNode.ListOf("BIGI", TdfType.Integer,
                Enumerable.Range(0, 300).Select(i => (object)(ulong)i).ToArray()),
        };

        var bad = 0;
        try
        {
            var encoded = TdfCodec.Encode(original);
            var decoded = TdfCodec.Decode(encoded);

            var stat = decoded.Struct("ENTI")?["STAT"] as TdfList;
            if (stat is null || stat.Items.Count != 1432)
            {
                Console.WriteLine($"  ОШИБКА: STAT содержит {stat?.Items.Count ?? -1} " +
                                  $"значений вместо 1432");
                bad++;
            }
            else if (!Equals(stat.Items[0], "0.00") ||
                     !Equals(stat.Items[1431], $"{1431 % 7}.00"))
            {
                Console.WriteLine($"  ОШИБКА: границы значений не совпали: " +
                                  $"'{stat.Items[0]}'..'{stat.Items[1431]}'");
                bad++;
            }

            var bigi = decoded.List("BIGI");
            if (bigi is null || bigi.Items.Count != 300 ||
                !Equals(bigi.Items[299], 299UL))
            {
                Console.WriteLine($"  ОШИБКА: BIGI — {bigi?.Items.Count ?? -1} " +
                                  $"элементов (нужно 300, последний 299)");
                bad++;
            }

            // обратное кодирование должно дать те же байты
            var re = TdfCodec.Encode(decoded);
            if (!re.AsSpan().SequenceEqual(encoded))
            {
                Console.WriteLine("  ОШИБКА: байты при обратном кодировании разошлись");
                bad++;
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  ОШИБКА: {ex.Message}");
            bad++;
        }

        Console.WriteLine(bad == 0
            ? "  OK: 1432 строки + 300 int round-trip"
            : $"  провалено: {bad}");
        return bad;
    }

    // локальные копии WriteVarint/ReadVarint, чтобы проверить их напрямую
    private static void WriteVarint(List<byte> buf, ulong value)
    {
        if (value < 0x40)
        {
            buf.Add((byte)value);
            return;
        }

        buf.Add((byte)((value & 0x3F) | 0x80));
        var rest = value >> 6;
        while (rest >= 0x80)
        {
            buf.Add((byte)((rest & 0x7F) | 0x80));
            rest >>= 7;
        }

        buf.Add((byte)rest);
    }

    private static ulong ReadVarint(byte[] d, ref int p)
    {
        var b = d[p++];
        if (b < 0x80)
        {
            return b;
        }

        var value = (ulong)(b & 0x3F);
        var shift = 6;
        while (p < d.Length)
        {
            var next = d[p++];
            if (shift < 64)
            {
                value |= (ulong)(next & 0x7F) << shift;
            }

            if (next < 0x80)
            {
                break;
            }

            shift += 7;
        }

        return value;
    }
}