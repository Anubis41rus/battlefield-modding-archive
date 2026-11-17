using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Windows;
using EVBots.Core;
using EVBots.Installation;

namespace EVBots.Launcher;

public partial class MainWindow
{
    private async void Join_Click(object sender, RoutedEventArgs e)
    {
        if (_busy)
        {
            // 29.10: раньше клик молча терялся (прогон 248) — теперь вход встаёт в очередь за подъёмом сервера
            if (_hosting) { _joinAfterHost = true; SetStatus("Сервер ещё поднимается — войду в игру сам, как только он будет готов."); }
            else SetStatus("Подожди, предыдущее действие ещё идёт.");
            return;
        }
        SaveUi();
        if (_authIp.Length == 0)
        { SetStatus("Сначала войди (Шаг 1)."); return; }
        if (_s.Login.Length == 0 || _s.Password.Length == 0)
        { SetStatus("Впишите логин и пароль."); return; }
        if (ClientLaunchRunning || HostRunner.ClientRunning())
        { SetStatus("Игра уже запущена."); return; }
        var ip = _authIp;
        // Клиент идёт туда же, куда бэкенд отдаёт адрес сервера (LAN-IP хоста), а не на 127.0.0.1.
        // В логе 17.10 адрес объявления (VPN 10.6.7.1) и адрес входа не совпали, и клиент рвал сессию Blaze после preAuth.
        var joinIp = ip == LocalIp ? (LanInfo.DefaultIp() ?? LocalIp) : ip;
        var gamePort = 25300;
        _busy = true;
        try
        {
            SetStatus("Вход на " + ip + "...");
            // 23.10: свой evbots-loader.exe (клиенту activation.dll первой); в старой установке — activation-loader.exe
            var loader = EVBots.Core.Paths.FindLoader(Path.Combine(_p.StandDir, "activation"));
            if (loader is null)
            { SetStatus("Нет activation\\evbots-loader.exe в папке EVBots. Нажми «Указать папку EVBots…» и «Проверить файлы»."); return; }
            if (_p.ClientGameDir.Length == 0 || !File.Exists(Path.Combine(_p.ClientGameDir, "bf3.exe")))
            { SetStatus("Не найден bf3.exe (игра). Положи папку Battlefield 3 рядом с лаунчером."); return; }
            var baseUrl = $"http://{ip}:8080";
            string gid = "";
            string serverMap = "", serverMode = "";
            var freq = 30;
            var stxt = await Http.GetStringAsync(baseUrl + "/servers");
            using (var d = JsonDocument.Parse(stxt))
                if (d.RootElement.TryGetProperty("servers", out var arr))
                    foreach (var s in arr.EnumerateArray())
                        if (s.TryGetProperty("finalized", out var f) && f.ValueKind == JsonValueKind.True
                            && s.TryGetProperty("gameId", out var g))
                        {
                            gid = g.GetRawText().Trim('"');
                            if (s.TryGetProperty("map", out var mp)) serverMap = mp.GetString() ?? "";
                            if (s.TryGetProperty("mode", out var md)) serverMode = md.GetString() ?? "";
                            if (s.TryGetProperty("address", out var address) && address.ValueKind == JsonValueKind.String &&
                                address.GetString() is { } addressText && IPEndPoint.TryParse(addressText, out var endpoint) && endpoint.Port > 0 &&
                                endpoint.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork)
                            {
                                gamePort = endpoint.Port;
                                if (ip == LocalIp) joinIp = endpoint.Address.ToString();
                            }
                            // 26.10: частота сервера (нет поля — старый бэкенд, 30 Гц); клиент ставит ту же, как у VU
                            if (s.TryGetProperty("frequency", out var fq) && fq.ValueKind == JsonValueKind.Number
                                && fq.TryGetInt32(out var hz))
                                freq = HostRunner.NormFrequency(hz);
                            break;
                        }
            if (gid.Length == 0) { SetStatus("У хоста нет готового сервера."); return; }
            SetStatus("Проверяю активные моды хоста...");
            var active = await ModProfiles.ReadHostAsync(Http, baseUrl);
            if ((_host.ServerAlive || HostRunner.AnyGameRunning()) && active.Profile.Id != SelectedProfile.Id)
                throw new InvalidOperationException("На этом ПК уже запущен сервер с другим модом. Сначала останови его.");
            if (active.Profile.IsRoyale && (!serverMap.Replace('\\', '/').Split('/').Last().Equals(ModProfiles.RoyaleMap, StringComparison.OrdinalIgnoreCase)
                || !serverMode.Equals(ModProfiles.RoyaleMode, StringComparison.Ordinal)))
                throw new InvalidDataException("VU Battle Royale требует XP5_003 / ConquestLarge0. Хост объявил " + serverMap + " / " + serverMode + ".");
            _remoteProfile = active.Profile;
            UpdateProfileControls();
            var localMods = await Task.Run(() => CheckClientMods(active));
            Log("Моды клиента совпадают с хостом: " + string.Join(", ", localMods.Mods.Select(m => m.Folder + " " + m.Version)));
            string? uiAssets = null;
            if (active.Profile.IsRoyale)
            {
                Log(ModProfiles.RoyaleNotice);
                SetStatus("Готовлю интерфейс VU Battle Royale из установленной игры...");
                uiAssets = await GameAssetCache.EnsureAsync(_p.ClientGameDir, Path.Combine(_p.StandDir, "cache", "ui-assets"), Log);
            }
            var persona = _persona;
            if (persona.Length == 0)
            {
                // personaref не выдан при входе — запрашиваем заново
                var nick = _s.Nick.Length > 0 ? _s.Nick : _s.Login;
                var body = JsonSerializer.Serialize(new { login = _s.Login, password = _s.Password, name = nick });
                var resp = await Http.PostAsync(baseUrl + "/accounts/ensure",
                    new StringContent(body, Encoding.UTF8, "application/json"));
                var rtxt = await resp.Content.ReadAsStringAsync();
                if (!resp.IsSuccessStatusCode)
                {
                    var why = "HTTP " + (int)resp.StatusCode;
                    try
                    {
                        using var ed = JsonDocument.Parse(rtxt);
                        if (ed.RootElement.TryGetProperty("error", out var er)) why = er.GetString() ?? why;
                    }
                    catch (JsonException) { }
                    SetStatus("Хост отклонил: " + why); Log("ERROR: " + why); return;
                }
                persona = ReadPersona(rtxt);
            }
            var dns = "gosredirector.ea.com,gosredirector.online.ea.com,gosredirector.scert.ea.com,gosredirector.stest.ea.com";
            // Кавычки здесь обычные: ArgumentList экранирует их сам. Двойной слэш даёт клиенту \" и вход рвётся (activation-лог 14:12).
            var pars = $"<data putinsquad=\"false\" gameid=\"{gid}\" personaref=\"{persona}\" levelmode=\"mp\" serveraddr=\"{joinIp}:{gamePort}\"/>";
            var psi = new ProcessStartInfo
            {
                FileName = loader,
                WorkingDirectory = _p.StandDir,
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            psi.Environment["BF3ACT_DNSREDIR"] = dns;
            psi.Environment["BF3ACT_DNSREDIR_IP"] = joinIp;
            psi.Environment["BF3ACT_DRM"] = "0";
            // 23.10 (К4): клиентская часть EVBots, как у VU — ev-core.dll (канал модов к серверу, порт 7948) и EVBots.dll
            // (клиентский Lua мода) после activation.dll. Только в новом комплекте (есть ev-core.dll). Имя игрока — для
            // канала (сервер сопоставляет соединение с игроком); моды клиента — из папки инстанса этого ПК, если она есть
            // (раздача модов с сервера будет позже).
            var act = Path.Combine(_p.StandDir, "activation");
            var clientDlls = new List<string>();
            foreach (var dll in new[] { "ev-core.dll", "EVBots.dll", "lua.dll" })
                if (!File.Exists(Path.Combine(act, dll))) throw new FileNotFoundException("Для модов клиента нужен activation\\" + dll);
            {
                clientDlls.AddRange(new[] { "--dll", "ev-core.dll", "--dll", "EVBots.dll" });
                psi.Environment["EVBOTS_PLAYER_NAME"] = _s.Nick.Length > 0 ? _s.Nick : _s.Login;
                // 26.10: частота движка клиента = частоте сервера (ev-core, EvFreq.cpp)
                psi.Environment["EVBOTS_FREQUENCY"] = freq.ToString();
                psi.Environment["EVBOTS_CLIENT_MODS"] = localMods.Directory;
                psi.Environment["EVBOTS_SETTINGS_DIR"] = Path.Combine(_p.StandDir, "mod-settings");
                psi.Environment["EVBOTS_ACTIVE_MODS"] = active.EnvironmentValue;
                psi.Environment.Remove("EVBOTS_UI_ASSETS");
                if (uiAssets is not null) psi.Environment["EVBOTS_UI_ASSETS"] = uiAssets;
            }
            var clientArgs = new List<string> { "--client", "--game", _p.ClientGameDir, "--drm-strict" };
            var license = Path.Combine(_p.StandDir, "license", "71067.dlf");
            if (File.Exists(license)) clientArgs.AddRange(new[] { "--license-artifact", license });
            foreach (var a in clientArgs.Concat(clientDlls).Concat(new[] {
                "--", "-webMode", "MP", "-Origin_NoAppFocus", "-onlineEnvironment", "prod",
                "-loginToken", "0", "-AuthToken", "0",
                "-requestState", "State_ClaimReservation", "-requestStateParams", pars,
                "-Online.BlazeLogLevel", "2", "-Online.DirtysockLogLevel", "2",
                "-windowed", "-w", "1280", "-h", "720",
            })) psi.ArgumentList.Add(a);
            Log($"join: login={_s.Login} gameid={gid} host={joinIp}" + (clientDlls.Count > 0 ? $" (клиент EVBots: ev-core + EVBots.dll, {freq} Гц)" : ""));
            if (HostRunner.ClientRunning()) throw new InvalidOperationException("Игра уже запущена.");
            _clientLoader?.Dispose();
            _clientLoader = Process.Start(psi) ?? throw new IOException("Не удалось запустить загрузчик клиента.");
            SetStatus(active.Profile.IsRoyale ? "Вход в VU Battle Royale запущен. Это тест оригинального мода." : "Вход запущен. В игре — DEPLOY.");
        }
        catch (HttpRequestException ex)
        {
            SetStatus($"Нет связи с {ip}:8080. Хост: «Фаервол (UAC)», потом «Остановить» и «Поднять сервер». Гость: проверь, что ПК в одной сети.");
            Log("ERROR: " + ex.Message);
        }
        catch (Exception ex) { SetStatus("Не удалось запустить вход: " + ex.Message); Log("ERROR: " + ex.Message); }
        finally { _busy = false; UpdateProfileControls(); }
    }

    /// <summary>
    /// Сеть одной командой cmd (UAC), на каждом ПК: и хосту, и гостю.
    /// 1) URL-ACL на HTTP бэкенда по LAN-адресу: без него бэкенд слушает только 127.0.0.1, гость не войдёт;
    /// 2) правила брандмауэра: TCP 8080, 42127, 10041 — вход и бэкенд; UDP 25300, 17554, 47990 — игра, QoS, объявления;
    /// 3) разрешение bf3.exe (и сервера, если он есть): игровой канал открывает сервер к клиенту.
    /// Окно скрыто, нужен только UAC.
    /// </summary>
    private async void Firewall_Click(object sender, RoutedEventArgs e)
    {
        var ip = MyLanIp();
        if (ip is null)
        { SetStatus("LAN-адрес этого ПК не найден (сеть отключена или только VPN). Подключи ПК к сети и повтори."); return; }
        var url = $"http://{ip}:8080/";
        var text = "Настроить сеть для игры (нужен UAC):\n" +
                   $"1) HTTP бэкенда для гостей: {url} (URL-ACL)\n" +
                   "2) порты: TCP 8080, 42127, 10041 — вход и бэкенд; UDP 25300, 17554, 47990 — игра, QoS, объявления\n" +
                   "3) разрешение bf3.exe" + (_p.ServerDir.Length > 0 ? " и сервера" : "") + " в брандмауэре\n\n" +
                   "Выполнить? Если сервер уже был запущен, потом останови его и подними заново.";
        if (MessageBox.Show(this, text, "Фаервол", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
            return;
        var user = Environment.UserName;
        var cmds = new List<string>
        {
            $"netsh http delete urlacl url={url} >nul 2>&1",
            $"netsh http add urlacl url={url} user=\"{user}\"",
            // 20.10: запись «+» перехватывает весь порт 8080 (HTTP 503) — удаляем, если её поставил установщик 20.10
            "netsh http delete urlacl url=http://+:8080/ >nul 2>&1",
            "netsh http delete urlacl url=http://127.0.0.1:8080/ >nul 2>&1",
            $"netsh http add urlacl url=http://127.0.0.1:8080/ user=\"{user}\"",
            "netsh advfirewall firewall delete rule name=\"EVBots-TCP\" >nul 2>&1",
            "netsh advfirewall firewall add rule name=\"EVBots-TCP\" dir=in action=allow protocol=TCP localport=8080,42127,10041,7948",  // 7948 — канал модов (К4)
            "netsh advfirewall firewall delete rule name=\"EVBots-UDP\" >nul 2>&1",
            "netsh advfirewall firewall add rule name=\"EVBots-UDP\" dir=in action=allow protocol=UDP localport=25300,17554,47990",
        };
        if (_p.ClientGameDir.Length > 0)
            AddProgramRule(cmds, "EVBots client", Path.Combine(_p.ClientGameDir, "bf3.exe"));
        if (_p.ServerDir.Length > 0)
            AddProgramRule(cmds, "EVBots server", Path.Combine(RoyaleInstanceConfig.ServerDirectory(_p.StandDir), "bf3_server_Final.exe"));
        var psi = new ProcessStartInfo("cmd.exe", "/d /c \"" + string.Join(" & ", cmds) + "\"")
        {
            Verb = "runas",
            UseShellExecute = true,
            WindowStyle = ProcessWindowStyle.Hidden,
        };
        try
        {
            await Task.Run(() =>
            {
                using var p = Process.Start(psi);
                p?.WaitForExit();
            });
            var acl = await Task.Run(() => UrlAclExists(ip));
            var fw = await Task.Run(() => FirewallRuleExists("EVBots-TCP") && FirewallRuleExists("EVBots-UDP"));
            Log(fw ? "Фаервол: правила EVBots-TCP и EVBots-UDP на месте." : "Фаервол: правил не видно — нажми ещё раз и подтверди UAC.");
            Log(acl ? $"HTTP для гостей: URL-ACL {url} на месте." : $"HTTP для гостей: URL-ACL {url} НЕ появился — нажми ещё раз и подтверди UAC.");
            SetStatus(acl && fw
                ? "Сеть настроена. Если сервер уже работал — «Остановить» и «Поднять сервер» заново."
                : "Сеть настроена не полностью — смотри лог.");
        }
        catch (System.ComponentModel.Win32Exception) { Log("Отказ от UAC."); SetStatus("Отказ от UAC: сеть не настроена."); }
    }

    /// <summary>Правило брандмауэра на программу (все порты этого exe), если файл есть.</summary>
    private static void AddProgramRule(List<string> cmds, string name, string exe)
    {
        if (!File.Exists(exe)) return;
        cmds.Add($"netsh advfirewall firewall delete rule name=\"{name}\" >nul 2>&1");
        cmds.Add($"netsh advfirewall firewall add rule name=\"{name}\" dir=in action=allow program=\"{exe}\" enable=yes");
    }

    /// <summary>Есть ли URL-ACL на HTTP бэкенда по этому адресу. Список netsh админ-прав не требует.</summary>
    internal static bool UrlAclExists(string ip)
    {
        var psi = new ProcessStartInfo("netsh", "http show urlacl")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };
        using var p = Process.Start(psi);
        if (p is null) return false;
        var output = p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        return output.Contains($"http://{ip}:8080/", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>Есть ли правило брандмауэра с таким именем (показ правил netsh не требует админ-прав).</summary>
    internal static bool FirewallRuleExists(string name)
    {
        var psi = new ProcessStartInfo("netsh", $"advfirewall firewall show rule name=\"{name}\"")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };
        using var p = Process.Start(psi);
        if (p is null) return false;
        var output = p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        return output.Contains(name, StringComparison.OrdinalIgnoreCase);
    }

    protected override void OnClosed(EventArgs e)
    {
        _s.Save();
        _lanLocal.Stop();
        _lan?.Dispose();
        base.OnClosed(e);
    }
}
