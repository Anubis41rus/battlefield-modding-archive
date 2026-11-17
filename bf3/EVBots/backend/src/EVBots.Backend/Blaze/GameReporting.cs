// GameReporting.cs — отчёты матча от сервера → статистика игроков (progress.json).
//
// 18.10.2026: статистика и звание не сохранялись между запусками — бэкенд отвечал
// на GameReporting (0x1C) пустым ответом и ничего не писал (progress.json последний
// раз обновлял старый бэкенд BF3NativeBots 11.10).
//
// Откуда формат (факт, не догадка):
//   * журналы старого бэкенда BF3NativeBots\profiles\<id>\reports\*.rpt (сырой payload
//     после 20-байтового заголовка BLRJ) и его разборщик (re\appsrc\...GameReportReader):
//     RPRT{ GAME: variable{ GAME{ PLYR: map<persona, { STAT: map<имя, число> }> } } };
//   * тип 7 в этом payload — «variable» (байт «есть», varint id типа, структура),
//     а не список чисел, как в остальном протоколе; float — big-endian.
//     Поэтому отчёт разбирается здесь, а не общим TdfCodec (тот на нём зацикливался 13.10);
//   * значения в отчёте — за матч (c___k_g = 4 при накопленных 45);
//   * как сливать — поле mode каталога статистик (bf3-stats-catalog.json, снят с
//     statgroups/bf3/bf3_player_mpdefault2.bin игры): increment — прибавить, max — максимум,
//     set — заменить. Имён вне каталога не пишем.

using System.Buffers.Binary;
using System.Globalization;
using EVBots.Backend.Data;

namespace EVBots.Backend.Blaze;

internal static class GameReporting
{
    // Учёт матча (прогон 18.10 15:52): в конце раунда сервер шлёт 0x64 и сразу 0x65 с ОДНИМИ И ТЕМИ ЖЕ значениями
    // «за матч до сих пор» — без учёта прирост считался бы дважды. Помним по игроку, сколько уже начислено в текущем
    // матче, и начисляем разницу. Значение меньше начисленного — новый матч/перезаход (счёт с нуля). После 0x65 матч закрыт.
    private static readonly Dictionary<ulong, Dictionary<string, double>> Ledger = new();
    private static readonly object LedgerGate = new();

    public const ushort SubmitGameReport = 0x01;
    public const ushort SubmitTrustedMidGameReport = 0x64;
    public const ushort SubmitTrustedEndGameReport = 0x65;

    /// <summary>Разбирает отчёт, сливает статистику игроков и отвечает пустым реплаем.</summary>
    public static void Handle(BlazeSession s, ushort cmd, int messageId, byte[] payload,
        ProfileStore store, StatsCatalog catalog, Log log)
    {
        try
        {
            if (cmd is SubmitGameReport or SubmitTrustedMidGameReport or SubmitTrustedEndGameReport)
            {
                Apply(s, cmd, payload, store, catalog, log);
            }
            else
            {
                log.Info($"[report] команда 0x{cmd:X} {payload.Length} байт — не отчёт, пустой ответ");
            }
        }
        catch (Exception ex)
        {
            log.Warn($"[report] отчёт 0x{cmd:X} ({payload.Length} байт) не разобран: {ex.Message}");
        }

        // сервер ждёт ответа на каждый запрос (иначе RPC_TIMEOUT и OFFLINE)
        s.Reply(Component.GameReporting, cmd, messageId, Array.Empty<Tdf.TdfNode>());
    }

    private static void Apply(BlazeSession s, ushort cmd, byte[] payload, ProfileStore store,
        StatsCatalog catalog, Log log)
    {
        var players = ReportReader.ReadPlayers(payload, out var finished);
        var kind = cmd == SubmitTrustedMidGameReport ? "в ходе матча" : "конец матча";
        log.Info($"[report] отчёт 0x{cmd:X} ({kind}, FNSH={finished}) {payload.Length} байт от сессии {s.Id}: " +
                 $"игроков {players.Count}");

        var modes = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var e in catalog.Stats)
        {
            modes[e.Name] = e.Mode;
        }

        foreach (var (persona, stats) in players)
        {
            if (store.Find(persona) is null)
            {
                log.Info($"[report]   {persona}: профиля нет — пропуск ({stats.Count} стат)");
                continue;
            }

            try
            {
                // сырой отчёт — в журнал игрока (как reports\*.rpt старого бэкенда), для разбора
                var dir = Path.Combine(store.Root, persona.ToString(CultureInfo.InvariantCulture), "reports");
                Directory.CreateDirectory(dir);
                File.WriteAllBytes(Path.Combine(dir, DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff",
                    CultureInfo.InvariantCulture) + $"-0x{cmd:X}.tdf"), payload);
            }
            catch (Exception ex)
            {
                log.Warn($"[report]   {persona}: журнал отчёта не записан: {ex.Message}");
            }

            var result = ApplyPlayer(persona, stats, modes, store, cmd != SubmitTrustedMidGameReport);
            result.Progress.Stats.TryGetValue("sc_general", out var general);
            log.Info($"[report]   {persona}: стат {stats.Count}, применено {result.Applied}, изменено {result.Changed}, " +
                     $"из них абсолютных (_a) {result.Absolute}, вне каталога {result.Unknown}; sc_general={general.ToString(CultureInfo.InvariantCulture)}");
        }
    }

    internal readonly record struct PlayerMerge(ProgressFile Progress, int Applied, int Changed, int Unknown, int Absolute);

    internal static PlayerMerge ApplyPlayer(ulong persona, Dictionary<string, double> stats,
        IReadOnlyDictionary<string, string> modes, ProfileStore store, bool endReport)
    {
        // Ledger и файл изменяются как одна операция. При ошибке записи начисление можно безопасно повторить.
        lock (LedgerGate)
        {
            var ledger = Ledger.TryGetValue(persona, out var previous)
                ? new Dictionary<string, double>(previous, StringComparer.Ordinal)
                : new Dictionary<string, double>(StringComparer.Ordinal);

            foreach (var (name, value) in stats)
            {
                if (double.IsFinite(value) && modes.TryGetValue(name, out var mode) && mode == "increment" &&
                    ledger.TryGetValue(name, out var was) && value < was - 1e-6)
                {
                    ledger.Clear();  // новый матч либо новый заход со счётом с нуля
                    break;
                }
            }

            int applied = 0, changed = 0, unknown = 0, absolute = 0;
            var progress = store.UpdateProgress(persona, current =>
            {
                // Абсолютные значения применяем после приростов.
                foreach (var (name, value) in stats.OrderBy(kv => kv.Key.EndsWith("_a", StringComparison.Ordinal) ? 1 : 0))
                {
                    var key = name;
                    var abs = false;
                    if (!modes.ContainsKey(name) && name.EndsWith("_a", StringComparison.Ordinal) &&
                        modes.ContainsKey(name[..^2]))
                    {
                        key = name[..^2];
                        abs = true;
                    }
                    if (!double.IsFinite(value) || !modes.TryGetValue(key, out var mode))
                    {
                        unknown++;
                        continue;
                    }

                    current.Stats.TryGetValue(key, out var old);
                    double nv;
                    if (abs)
                    {
                        nv = Math.Max(old, value);
                        absolute++;
                    }
                    else if (mode == "increment")
                    {
                        ledger.TryGetValue(name, out var was);
                        ledger[name] = value;
                        nv = old + Math.Max(0, value - was);
                    }
                    else
                    {
                        nv = mode switch
                        {
                            "max" => Math.Max(old, value),
                            "set" => value,
                            _ => old,
                        };
                    }
                    applied++;
                    if (nv != old)
                    {
                        current.Stats[key] = nv;
                        changed++;
                    }
                }
            });
            if (endReport) Ledger.Remove(persona);
            else Ledger[persona] = ledger;
            return new PlayerMerge(progress, applied, changed, unknown, absolute);
        }
    }
}

/// <summary>Минимальный разбор TDF отчёта (типы как в payload GameReporting BF3).</summary>
internal sealed class ReportReader
{
    private readonly byte[] d;
    private int p;

    private ReportReader(byte[] data) => d = data;

    public static List<(ulong Persona, Dictionary<string, double> Stats)> ReadPlayers(byte[] payload,
        out long finished)
    {
        var r = new ReportReader(payload);
        var top = r.ReadFields(untilEnd: true);
        finished = top.TryGetValue("FNSH", out var f) && f is long fl ? fl : -1;
        var result = new List<(ulong, Dictionary<string, double>)>();
        if (!top.TryGetValue("RPRT", out var rprt) || rprt is not Dictionary<string, object?> rp)
        {
            throw new InvalidDataException("нет RPRT");
        }

        // RPRT.GAME — variable → структура GAME → PLYR
        object? game = rp.GetValueOrDefault("GAME");
        if (game is Dictionary<string, object?> g1 && g1.GetValueOrDefault("GAME") is Dictionary<string, object?> g2)
        {
            game = g2;
        }

        if (game is not Dictionary<string, object?> gm || gm.GetValueOrDefault("PLYR") is not List<(object?, object?)> plyr)
        {
            throw new InvalidDataException("нет RPRT.GAME.PLYR");
        }

        foreach (var (k, v) in plyr)
        {
            if (k is not long id || id <= 0 || v is not Dictionary<string, object?> ps)
            {
                continue;
            }

            var stats = new Dictionary<string, double>(StringComparer.Ordinal);
            if (ps.GetValueOrDefault("STAT") is List<(object?, object?)> st)
            {
                foreach (var (sk, sv) in st)
                {
                    if (sk is not string name)
                    {
                        continue;
                    }

                    double? num = sv switch
                    {
                        float x => x,
                        long x => x,
                        string x when double.TryParse(x, NumberStyles.Float, CultureInfo.InvariantCulture, out var y) => y,
                        _ => null,
                    };
                    if (num is { } n)
                    {
                        stats[name] = n;
                    }
                }
            }

            result.Add(((ulong)id, stats));
        }

        return result;
    }

    private Dictionary<string, object?> ReadFields(bool untilEnd)
    {
        var o = new Dictionary<string, object?>(StringComparer.Ordinal);
        while (p < d.Length)
        {
            if (!untilEnd && d[p] == 0)
            {
                p++;
                break;
            }

            if (p + 4 > d.Length)
            {
                p = d.Length;
                break;
            }

            var label = Label();
            var type = d[p++];
            o[label] = Value(type);
        }

        return o;
    }

    private string Label()
    {
        var v = (d[p] << 16) | (d[p + 1] << 8) | d[p + 2];
        p += 3;
        var chars = new List<char>(4);
        for (var sh = 18; sh >= 0; sh -= 6)
        {
            var c = (v >> sh) & 0x3F;
            if (c != 0)
            {
                chars.Add((char)(c + 0x20));
            }
        }

        return new string(chars.ToArray());
    }

    private long VarInt()
    {
        int b = d[p++];
        var neg = (b & 0x40) != 0;
        long v = b & 0x3F;
        var sh = 6;
        while ((b & 0x80) != 0)
        {
            b = d[p++];
            v |= (long)(b & 0x7F) << sh;
            sh += 7;
        }

        return neg ? -v : v;
    }

    private object? Value(byte type)
    {
        switch (type)
        {
            case 0: return VarInt();
            case 1:
            {
                var n = (int)VarInt();
                var s = System.Text.Encoding.UTF8.GetString(d, p, n).TrimEnd('\0');
                p += n;
                return s;
            }
            case 2:
            {
                var n = (int)VarInt();
                p += n;
                return null;
            }
            case 3: return ReadFields(untilEnd: false);
            case 4:
            {
                var et = d[p++];
                var n = (int)VarInt();
                var l = new List<object?>(n);
                for (var i = 0; i < n; i++)
                {
                    l.Add(Value(et));
                }

                return l;
            }
            case 5:
            {
                var kt = d[p];
                var vt = d[p + 1];
                p += 2;
                var n = (int)VarInt();
                var m = new List<(object?, object?)>(n);
                for (var i = 0; i < n; i++)
                {
                    var k = Value(kt);
                    m.Add((k, Value(vt)));
                }

                return m;
            }
            case 6:
            {
                var active = d[p++];
                if (active == 0x7F)
                {
                    return null;
                }

                var label = Label();
                var t = d[p++];
                return new Dictionary<string, object?> { [label] = Value(t) };
            }
            case 7:
            {
                // variable: «есть» (байт), id типа (varint), структура
                var present = d[p++];
                if (present == 0)
                {
                    return null;
                }

                VarInt();
                return ReadFields(untilEnd: false);
            }
            case 8: VarInt(); VarInt(); return null;
            case 9: VarInt(); VarInt(); VarInt(); return null;
            case 0x0A:
            {
                var f = BinaryPrimitives.ReadSingleBigEndian(d.AsSpan(p, 4));
                p += 4;
                return f;
            }
            case 0x0B: return VarInt();
            default: throw new InvalidDataException($"тип {type} на {p}");
        }
    }
}
