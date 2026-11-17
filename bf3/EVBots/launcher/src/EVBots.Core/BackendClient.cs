using System.Net.Http;
using System.Text;
using System.Text.Json;

namespace EVBots.Core;

/// <summary>Ответ HTTP-API бэкенда. Error — короткое сообщение по-русски при неудаче.</summary>
public sealed class ApiResponse
{
    public bool Ok { get; init; }
    public int Status { get; init; }
    public string Body { get; init; } = "";
    public string Error { get; init; } = "";
}

/// <summary>Аккаунт из GET /accounts (секретов нет).</summary>
public sealed record AccountDto(string Login, ulong Personaref, string Name, int Soldiers);

/// <summary>Живой сервер из GET /servers.</summary>
public sealed record GameDto(ulong GameId, string Name, string Address, string Map, string Mode, int Players, int MaxPlayers, string State);

/// <summary>
/// HTTP-клиент бэкенда. Без системного прокси: прокси VPN ломает обращения к LAN и 127.0.0.1.
/// Разбор JSON вынесен в статические методы, чтобы их можно было проверить без сети.
/// </summary>
public sealed class BackendClient
{
    private static readonly HttpClient Http = new(new HttpClientHandler { UseProxy = false });

    public BackendClient(string host, int port)
    {
        Host = host;
        Port = port;
        Root = $"http://{host}:{port}";
    }

    public string Host { get; }
    public int Port { get; }
    public string Root { get; }

    public Task<ApiResponse> GetAsync(string path) => SendAsync(HttpMethod.Get, path, null);

    public Task<ApiResponse> PostAsync(string path, object body) => SendAsync(HttpMethod.Post, path, body);

    public Task<ApiResponse> DeleteAsync(string path) => SendAsync(HttpMethod.Delete, path, null);

    /// <summary>true, если по адресу отвечает именно EV Backend (GET / → service = EV Backend).</summary>
    public async Task<bool> IsLauncherBackendAsync()
    {
        var r = await GetAsync("/");
        if (!r.Ok)
        {
            return false;
        }

        try
        {
            using var doc = JsonDocument.Parse(r.Body);
            return doc.RootElement.TryGetProperty("service", out var s) && s.GetString() == "EV Backend";
        }
        catch (JsonException)
        {
            return false;
        }
    }

    public async Task<(bool Ok, List<AccountDto> Items, string Error)> AccountsAsync()
    {
        var r = await GetAsync("/accounts");
        return r.Ok ? (true, ParseAccounts(r.Body), "") : (false, new List<AccountDto>(), r.Error);
    }

    public async Task<(bool Ok, List<GameDto> Items, string Error)> ServersAsync()
    {
        var r = await GetAsync("/servers");
        return r.Ok ? (true, ParseServers(r.Body), "") : (false, new List<GameDto>(), r.Error);
    }

    /// <summary>POST /accounts/ensure: войти или зарегистрировать. personaref назначает бэкенд.</summary>
    public async Task<(bool Ok, ulong Personaref, string Error)> EnsureAccountAsync(string login, string password, string? name)
    {
        var body = new Dictionary<string, object?> { ["login"] = login, ["password"] = password };
        if (!string.IsNullOrWhiteSpace(name))
        {
            body["name"] = name;
        }

        var r = await PostAsync("/accounts/ensure", body);
        if (!r.Ok)
        {
            return (false, 0, r.Error);
        }

        using var doc = JsonDocument.Parse(r.Body);
        return (true, U64(doc.RootElement, "personaref"), "");
    }

    /// <summary>DELETE /accounts/{login}. keepSoldier=true сохраняет солдата и его прогресс.</summary>
    public Task<ApiResponse> DeleteAccountAsync(string login, bool keepSoldier) =>
        DeleteAsync($"/accounts/{Uri.EscapeDataString(login)}" + (keepSoldier ? "?keepSoldier=1" : ""));

    /// <summary>POST /admin/startup — пишет параметр в Admin\Startup.txt инстанса.</summary>
    public Task<ApiResponse> SetStartupAsync(string key, string value) =>
        PostAsync("/admin/startup", new Dictionary<string, object?> { ["key"] = key, ["value"] = value });

    /// <summary>POST /admin/maplist — пишет ротацию в Admin\MapList.txt (одна карта).</summary>
    public Task<ApiResponse> SetMapListAsync(string map, string mode, int rounds) =>
        PostAsync("/admin/maplist", new { entries = new[] { new { map, gamemode = mode, rounds } } });

    private async Task<ApiResponse> SendAsync(HttpMethod method, string path, object? body)
    {
        using var req = new HttpRequestMessage(method, Root + path);
        if (body is not null)
        {
            req.Content = new StringContent(JsonSerializer.Serialize(body), Encoding.UTF8, "application/json");
        }

        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(4));
        try
        {
            using var resp = await Http.SendAsync(req, cts.Token);
            var text = await resp.Content.ReadAsStringAsync(cts.Token);
            var status = (int)resp.StatusCode;
            var ok = resp.IsSuccessStatusCode;
            return new ApiResponse
            {
                Ok = ok,
                Status = status,
                Body = text,
                Error = ok ? "" : ErrorText(text, status),
            };
        }
        catch (OperationCanceledException)
        {
            return Fail($"нет ответа от {Root} (таймаут)");
        }
        catch (HttpRequestException)
        {
            return Fail($"нет связи с {Root}");
        }
    }

    private ApiResponse Fail(string error) => new() { Ok = false, Status = 0, Error = error };

    /// <summary>Текст ошибки: поле error из JSON-ответа, иначе код HTTP.</summary>
    public static string ErrorText(string body, int status)
    {
        try
        {
            using var doc = JsonDocument.Parse(body);
            if (doc.RootElement.TryGetProperty("error", out var e) && e.ValueKind == JsonValueKind.String)
            {
                return e.GetString() ?? $"HTTP {status}";
            }
        }
        catch (JsonException)
        {
            // не JSON — покажем код
        }

        return $"HTTP {status}";
    }

    public static List<AccountDto> ParseAccounts(string json)
    {
        var list = new List<AccountDto>();
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.TryGetProperty("accounts", out var arr) && arr.ValueKind == JsonValueKind.Array)
        {
            foreach (var a in arr.EnumerateArray())
            {
                list.Add(new AccountDto(Str(a, "login"), U64(a, "personaref"), Str(a, "name"), Int(a, "soldiers")));
            }
        }

        return list;
    }

    public static List<GameDto> ParseServers(string json)
    {
        var list = new List<GameDto>();
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.TryGetProperty("servers", out var arr) && arr.ValueKind == JsonValueKind.Array)
        {
            foreach (var s in arr.EnumerateArray())
            {
                list.Add(new GameDto(
                    U64(s, "gameId"),
                    Str(s, "name"),
                    Str(s, "address"),
                    Str(s, "map"),
                    Str(s, "mode"),
                    Int(s, "players"),
                    Int(s, "maxPlayers"),
                    Raw(s, "state")));
            }
        }

        return list;
    }

    private static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static string Raw(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) ? (v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.GetRawText()) : "";

    private static ulong U64(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetUInt64(out var x) ? x : 0;

    private static int Int(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var x) ? x : 0;
}
