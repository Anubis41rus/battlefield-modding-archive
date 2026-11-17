// Шаг 1 — вход или регистрация. Только после этого появляются
// «поднять сервер» и «войти в игру».
//
// Куда ходим: сервер из списка «Сервер» (он находится в сети сам) или IP, вписанный вручную.
//   «Этот ПК» (127.0.0.1) = я хост: перед входом сам поднимаю бэкенд (если ещё не бежит).
//   чужой IP = гость: хожу на бэкенд друга, свой бэкенд не нужен.
//
// Маршруты бэкенда (WebHost.cs):
//   POST /accounts/login   {"login","password"}           -> вход в существующий
//   POST /accounts/ensure  {"login","password","name"}    -> войти или зарегистрировать
// Бэкенд возвращает personaref — он подставляется в requestStateParams
// при запуске игры, чтобы гость играл под своим солдатом.

using System.IO;
using EVBots.Installation;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Windows;

namespace EVBots.Launcher;

public partial class MainWindow
{
    /// <summary>IP бэкенда, на котором вошли (хост или я сам).</summary>
    private string _authIp = "";

    /// <summary>personaref аккаунта, полученный при входе.</summary>
    private string _persona = "";

    private async void Login_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        SaveUi();
        if (_s.Login.Length < 3)
        {
            SetStatus("Логин: минимум 3 символа.");
            return;
        }
        if (_s.Password.Length < 4)
        {
            SetStatus("Пароль: минимум 4 символа.");
            return;
        }
        var ip = AuthIpFromUi();
        if (ip.Length == 0)
        {
            SetStatus("Впиши IP сервера друга или выбери сервер в списке.");
            return;
        }
        _busy = true;
        AuthEnabled(false);
        try
        {
            SetStatus("Проверяю аккаунт на " + ip + "...");
            if (ip == LocalIp && !await EnsureLocalBackendAsync())
            {
                SetStatus("Бэкенд не поднялся: нет .NET 10, занят порт 8080 или нет прав URL-ACL. Смотри лог.");
                return;
            }
            // 01.11: ник шлём и при входе — бэкенд переименует солдата, если ник в поле поменяли
            var body = JsonSerializer.Serialize(new { login = _s.Login, password = _s.Password, name = _s.Nick.Length > 0 ? _s.Nick : null });
            using var resp = await Http.PostAsync($"http://{ip}:8080/accounts/login",
                new StringContent(body, Encoding.UTF8, "application/json"));
            var txt = await resp.Content.ReadAsStringAsync();
            if (!resp.IsSuccessStatusCode)
            {
                var why = BackendError(txt, "HTTP " + (int)resp.StatusCode);
                if (why.Contains("не найден"))
                    SetStatus("Нет такого аккаунта. Нажми «Регистрация».");
                else
                    SetStatus("Вход отклонён: " + why);
                return;
            }
            _persona = ReadPersona(txt);
            var renameError = TakeGameName(txt);
            Log($"вход: login={_s.Login} nick={_s.Nick} persona={_persona} на {ip}");
            EnterMain(ip);
            if (renameError is not null) SetStatus(renameError);
        }
        catch (Exception ex)
        {
            SetStatus(ip == LocalIp ? "Бэкенд не отвечает." : $"Нет связи с {ip}:8080. Хост: «Фаервол (UAC)», потом «Остановить» и «Поднять сервер». Гость: проверь IP и сеть.");
            Log("ERROR: " + ex.Message);
        }
        finally
        {
            _busy = false;
            AuthEnabled(true);
        }
    }

    private async void Register_Click(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        SaveUi();
        if (_s.Login.Length < 3)
        {
            SetStatus("Логин: минимум 3 символа.");
            return;
        }
        if (_s.Password.Length < 4)
        {
            SetStatus("Пароль: минимум 4 символа.");
            return;
        }
        var ip = AuthIpFromUi();
        if (ip.Length == 0)
        {
            SetStatus("Впиши IP сервера друга или выбери сервер в списке.");
            return;
        }
        _busy = true;
        AuthEnabled(false);
        try
        {
            SetStatus("Регистрирую на " + ip + "...");
            if (ip == LocalIp && !await EnsureLocalBackendAsync())
            {
                SetStatus("Бэкенд не поднялся: нет .NET 10, занят порт 8080 или нет прав URL-ACL. Смотри лог.");
                return;
            }
            var nick = _s.Nick.Length > 0 ? _s.Nick : _s.Login;
            var body = JsonSerializer.Serialize(new { login = _s.Login, password = _s.Password, name = nick });
            using var resp = await Http.PostAsync($"http://{ip}:8080/accounts/ensure",
                new StringContent(body, Encoding.UTF8, "application/json"));
            var txt = await resp.Content.ReadAsStringAsync();
            if (!resp.IsSuccessStatusCode)
            {
                SetStatus("Регистрация отклонена: " + BackendError(txt, "HTTP " + (int)resp.StatusCode));
                return;
            }
            _persona = ReadPersona(txt);
            _s.Nick = nick;
            var renameError = TakeGameName(txt);
            Log($"регистрация: login={_s.Login} nick={_s.Nick} persona={_persona} на {ip}");
            EnterMain(ip);
            if (renameError is not null) SetStatus(renameError);
        }
        catch (Exception ex)
        {
            SetStatus(ip == LocalIp ? "Бэкенд не отвечает." : $"Нет связи с {ip}:8080. Хост: «Фаервол (UAC)», потом «Остановить» и «Поднять сервер». Гость: проверь IP и сеть.");
            Log("ERROR: " + ex.Message);
        }
        finally
        {
            _busy = false;
            AuthEnabled(true);
        }
    }

    private void Logout_Click(object sender, RoutedEventArgs e)
    {
        _tick.Stop();
        _authIp = "";
        _persona = "";
        MainPanel.Visibility = Visibility.Collapsed;
        AuthPanel.Visibility = Visibility.Visible;
        SetStatus("Вышли. Войди заново.");
    }

    private void AuthEnabled(bool on)
    {
        LoginBtn.IsEnabled = on;
        RegisterBtn.IsEnabled = on;
    }

    /// <summary>IP для входа: сервер из списка «Сервер»; при «Ввести IP вручную…» — то, что вписано.</summary>
    private string AuthIpFromUi()
    {
        var pick = ServerPick.SelectedItem as PickItem;
        if (pick is null) return LocalIp;
        return pick.Manual ? HostBox.Text.Trim() : pick.Ip;
    }

    /// <summary>Уход на экран 2 (после успешного входа/регистрации).</summary>
    private void EnterMain(string ip)
    {
        _authIp = ip;
        _s.HostIp = ip;
        _s.Save();
        // Лог сервера показываем только с этого момента: строки прошлых прогонов не выводим
        var logPath = Path.Combine(_p.StandDir, "logs", "EVBots.log");
        _logOffset = File.Exists(logPath) ? new FileInfo(logPath).Length : 0;
        AuthPanel.Visibility = Visibility.Collapsed;
        MainPanel.Visibility = Visibility.Visible;
        var role = ip == LocalIp ? "хост" : "гость";
        AccountText.Text = $"{_s.Nick} ({_s.Login}) · {role} · {ip}";
        HostGroup.Visibility = ip == LocalIp ? Visibility.Visible : Visibility.Collapsed;
        _remoteProfile = null;
        UpdateProfileControls();
        _ = ReadRemoteProfileLabelAsync();
        SetStatus("Ты вошёл. Дальше: поднять сервер или войти в игру.");
        _ = TickAsync();
        _ = SyncProgressAsync(true);  // 18.10: своя статистика — на хост и обратно
        _tick.Start();
    }

    /// <summary>Хост: бэкенд должен бежать до входа — поднимаю, если его нет.</summary>
    private async Task<bool> EnsureLocalBackendAsync()
    {
        if (await PingAsync(LocalIp)) return true;
        SetStatus("Запускаю бэкенд...");
        try
        {
            await Task.Run(() =>
            {
                var active = CheckHostMods(SelectedProfile);
                var instance = RoyaleInstanceConfig.InstanceDirectory(_p.StandDir);
                RoyaleInstanceConfig.Write(instance, _s.ServerName);
                _host.StartBackend(_p, MyLanIp() ?? "", _s.ServerName, Log, _s.Frequency, active.Directory, instance);
            });
        }
        catch (Exception ex)
        {
            Log("ERROR backend: " + ex.Message);
            return false;
        }
        for (var i = 0; i < 20; i++)
        {
            await Task.Delay(1000);
            if (await PingAsync(LocalIp)) return true;
        }
        return false;
    }

    private async Task<bool> PingAsync(string ip)
    {
        try
        {
            await Http.GetStringAsync($"http://{ip}:8080/");
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private static string BackendError(string body, string fallback)
    {
        try
        {
            using var d = JsonDocument.Parse(body);
            if (d.RootElement.TryGetProperty("error", out var e))
                return e.GetString() ?? fallback;
        }
        catch (JsonException) { }
        return fallback;
    }

    /// <summary>
    /// 01.11: имя солдата в игре — из ответа бэкенда (name), а не из поля. По нему лаунчер выдаёт права меню F12
    /// (funbots.Permissions), и мод сверяет их с именем игрока: поле с другим ником давало «нет прав» у хоста.
    /// Возвращает текст ошибки переименования (ник занят) или null.
    /// </summary>
    private string? TakeGameName(string body)
    {
        try
        {
            using var d = JsonDocument.Parse(body);
            var root = d.RootElement;
            if (root.TryGetProperty("name", out var n) && n.ValueKind == JsonValueKind.String
                && (n.GetString() ?? "").Trim() is { Length: > 0 } name)
            {
                _s.Nick = name;
                NickBox.Text = name;
                _s.Save();
            }
            if (root.TryGetProperty("renameError", out var e) && e.ValueKind == JsonValueKind.String)
            {
                Log("ник: " + e.GetString());
                return e.GetString();
            }
        }
        catch (JsonException) { }
        return null;
    }

    private static string ReadPersona(string body)
    {
        try
        {
            using var d = JsonDocument.Parse(body);
            if (d.RootElement.TryGetProperty("personaref", out var p))
                return p.GetRawText().Trim('"');
        }
        catch (JsonException) { }
        return "";
    }
}
