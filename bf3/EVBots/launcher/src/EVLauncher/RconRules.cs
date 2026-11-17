// Тикеты/очки (и время для «Захвата флага») по режимам через RCON сервера (20.10.2026).
// Проверено на стенде 20.10: vars.gameModeCounter, поставленный посреди раунда, текущий раунд не меняет — действует со
// следующей загрузки уровня. Поэтому значения СЛЕДУЮЩЕГО раунда ставим сразу при подключении и после каждого
// server.onLevelLoaded. Первую карту задаёт Startup.txt (Host_Click). Времени раунда в BF3 нет (vars.roundTimeLimit —
// UnknownCommand), кроме CTF: vars.ctfRoundTimeModifier.
// Протокол Frostbite RCON — как в EVBots.dll (VextRcon.cpp): u32 заголовок (бит 31 — от сервера, бит 30 — ответ, 0..29 —
// номер), u32 размер пакета, u32 число слов, слова: u32 длина, байты, '\0'. Вход: login.plainText <пароль>.
// События — отдельное соединение (admin.eventsEnabled true), команды — короткое соединение на каждую.
using System.IO;
using System.Net.Sockets;
using System.Text;

namespace EVBots.Launcher;

internal sealed class RconRules
{
    private const string Host = "127.0.0.1";
    private const int Port = 47900;
    private readonly string _password;
    private readonly Func<string, ModeRule> _rule;
    private readonly Action<string> _log;
    private readonly CancellationTokenSource _stop = new();
    private uint _seq = 1;

    public RconRules(string password, Func<string, ModeRule> rule, Action<string> log)
    {
        _password = password;
        _rule = rule;
        _log = log;
    }

    public void Start() => _ = Task.Run(() => EventsLoopAsync(_stop.Token));

    private NetworkStream? _events;

    /// <summary>
    /// 20.10: перед разрывом — «quit»: сервер сам закрывает соединение (без этого в его журнале
    /// «SocketStreamTcp: Socket error: WSAECONNRESET»).
    /// </summary>
    public void Stop()
    {
        try
        {
            var s = _events;
            if (s != null)
                WriteAsync(s, CancellationToken.None, Interlocked.Increment(ref _seq) & 0x3FFFFFFFu, "quit").Wait(1000);
        }
        catch { }
        try { _stop.Cancel(); } catch { }
    }

    private async Task EventsLoopAsync(CancellationToken ct)
    {
        var said = false;
        while (!ct.IsCancellationRequested)
        {
            try
            {
                using var c = new TcpClient();
                await c.ConnectAsync(Host, Port, ct);
                var s = c.GetStream();
                if (!Ok(await ExchangeAsync(s, ct, "login.plainText", _password))) { _log("RCON: вход не принят (пароль?)"); await Task.Delay(10000, ct); continue; }
                if (!Ok(await ExchangeAsync(s, ct, "admin.eventsEnabled", "true"))) { _log("RCON: события не включились"); await Task.Delay(10000, ct); continue; }
                if (!said) { _log("RCON: тикеты по режимам включены (значения следующей карты ставлю заранее)"); said = true; }
                _events = s;
                // идущий раунд это не меняет: значение действует со следующей загрузки уровня (проверено 20.10 на стенде)
                _ = Task.Run(() => ApplyNextAsync(ct));
                while (!ct.IsCancellationRequested)
                {
                    var (hdr, words) = await ReadAsync(s, ct);
                    var fromServer = (hdr & 0x80000000u) != 0;
                    var isResponse = (hdr & 0x40000000u) != 0;
                    if (!fromServer || isResponse) continue;
                    // событие сервера: подтверждаем, как требует протокол
                    await WriteAsync(s, ct, (hdr & 0x3FFFFFFFu) | 0x80000000u | 0x40000000u, "OK");
                    // после загрузки уровня — сразу значения для следующего раунда (работает и при mapList.runNextRound,
                    // который server.onRoundOver не присылает)
                    if (words.Length > 0 && words[0] == "server.onLevelLoaded")
                        _ = Task.Run(() => ApplyNextAsync(ct));
                }
            }
            catch (OperationCanceledException) { return; }
            catch (Exception)
            {
                // сервер ещё не поднялся или перезапускается — пробуем снова
                try { await Task.Delay(5000, ct); } catch (OperationCanceledException) { return; }
            }
        }
    }

    /// <summary>Режим следующего раунда: та же карта, если раунды на ней не кончились, иначе следующая в MapList.</summary>
    private async Task ApplyNextAsync(CancellationToken ct)
    {
        try
        {
            var info = await CommandAsync(ct, "serverInfo");
            if (!Ok(info) || info.Length < 8) { _log("RCON: serverInfo не прочитан — тикеты не ставлю"); return; }
            var mode = info[4];
            int.TryParse(info[6], out var played);
            int.TryParse(info[7], out var total);
            var next = mode;
            if (played + 1 >= total)
            {
                var idx = await CommandAsync(ct, "mapList.getMapIndices");
                var list = await CommandAsync(ct, "mapList.list");
                if (Ok(idx) && idx.Length >= 3 && Ok(list) && list.Length >= 3 &&
                    int.TryParse(idx[2], out var ni) && int.TryParse(list[1], out var n) && int.TryParse(list[2], out var w) &&
                    w >= 2 && ni >= 0 && ni < n && list.Length >= 3 + (ni + 1) * w)
                    next = list[3 + ni * w + 1];
            }
            var r = _rule(next);
            var a = await CommandAsync(ct, "vars.gameModeCounter", r.Tickets.ToString());
            // времени раунда в BF3 нет, кроме «Захвата флага»: vars.ctfRoundTimeModifier (в exe сервера нет roundTimeLimit)
            var time = IsCtf(next) ? r.Time : 100;
            var b = await CommandAsync(ct, "vars.ctfRoundTimeModifier", time.ToString());
            _log($"RCON: следующий раунд — {next}: тикеты/очки {r.Tickets}% ({(Ok(a) ? "ок" : string.Join(' ', a))})" +
                 (IsCtf(next) ? $", время CTF {time}% ({(Ok(b) ? "ок" : string.Join(' ', b))})" : ""));
        }
        catch (Exception ex) { _log("RCON: не удалось поставить тикеты: " + ex.Message); }
    }

    /// <summary>22.10: список карт сменился на ходу — тикеты/очки следующей карты поставить сразу.</summary>
    public void ApplyNext() => _ = Task.Run(() => ApplyNextAsync(_stop.Token));

    /// <summary>Одна команда RCON (вход, команда, quit). Для лаунчера — например, команда мода.</summary>
    public Task<string[]> SendAsync(params string[] words) => CommandAsync(_stop.Token, words);

    private async Task<string[]> CommandAsync(CancellationToken ct, params string[] words)
    {
        using var c = new TcpClient();
        await c.ConnectAsync(Host, Port, ct);
        var s = c.GetStream();
        if (!Ok(await ExchangeAsync(s, ct, "login.plainText", _password))) return new[] { "LoginFailed" };
        var resp = await ExchangeAsync(s, ct, words);
        // 20.10: выход командой quit — соединение закрывает сервер, без WSAECONNRESET в его журнале
        try { await ExchangeAsync(s, ct, "quit"); } catch { }
        return resp;
    }

    private async Task<string[]> ExchangeAsync(NetworkStream s, CancellationToken ct, params string[] words)
    {
        var seq = Interlocked.Increment(ref _seq) & 0x3FFFFFFFu;
        await WriteAsync(s, ct, seq, words);
        using var to = CancellationTokenSource.CreateLinkedTokenSource(ct);
        to.CancelAfter(5000);
        while (true)
        {
            var (hdr, resp) = await ReadAsync(s, to.Token);
            var fromServer = (hdr & 0x80000000u) != 0;
            var isResponse = (hdr & 0x40000000u) != 0;
            if (!fromServer && isResponse && (hdr & 0x3FFFFFFFu) == seq) return resp;
        }
    }

    internal static bool Ok(string[] r) => r.Length > 0 && r[0] == "OK";

    public static bool IsCtf(string mode) => mode.StartsWith("CaptureTheFlag", StringComparison.OrdinalIgnoreCase);

    private static async Task WriteAsync(NetworkStream s, CancellationToken ct, uint hdr, params string[] words)
    {
        var body = new List<byte>();
        foreach (var w in words)
        {
            var b = Encoding.ASCII.GetBytes(w);
            body.AddRange(BitConverter.GetBytes((uint)b.Length));
            body.AddRange(b);
            body.Add(0);
        }
        var pkt = new byte[12 + body.Count];
        BitConverter.GetBytes(hdr).CopyTo(pkt, 0);
        BitConverter.GetBytes((uint)pkt.Length).CopyTo(pkt, 4);
        BitConverter.GetBytes((uint)words.Length).CopyTo(pkt, 8);
        body.CopyTo(pkt, 12);
        await s.WriteAsync(pkt, ct);
    }

    private static async Task<(uint hdr, string[] words)> ReadAsync(NetworkStream s, CancellationToken ct)
    {
        var h = await ReadExactAsync(s, 12, ct);
        var hdr = BitConverter.ToUInt32(h, 0);
        var size = BitConverter.ToUInt32(h, 4);
        var n = BitConverter.ToUInt32(h, 8);
        if (size < 12 || size > 16384) throw new IOException("RCON: неверный размер пакета");
        var b = await ReadExactAsync(s, (int)size - 12, ct);
        var words = new List<string>();
        var p = 0;
        for (var i = 0; i < n && p + 4 <= b.Length; i++)
        {
            var len = (int)BitConverter.ToUInt32(b, p);
            p += 4;
            if (p + len > b.Length) break;
            words.Add(Encoding.ASCII.GetString(b, p, len));
            p += len + 1;
        }
        return (hdr, words.ToArray());
    }

    private static async Task<byte[]> ReadExactAsync(NetworkStream s, int n, CancellationToken ct)
    {
        var buf = new byte[n];
        var got = 0;
        while (got < n)
        {
            var r = await s.ReadAsync(buf.AsMemory(got, n - got), ct);
            if (r <= 0) throw new IOException("RCON: соединение закрыто");
            got += r;
        }
        return buf;
    }
}
