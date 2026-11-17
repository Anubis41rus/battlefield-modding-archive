// ProtocolRoundTrip — сквозная проверка Blaze-протокола без игры.
//
// Поднимает слушатели на свободных портах и общается с ними настоящим
// TLS-клиентом: пишет кадры в сокет, читает ответы, разбирает TDF.
// Это проверяет цепочку целиком: разбор заголовка -> TDF -> диспетчер ->
// обработчик -> сборка ответа -> TLS -> сокет.
//
// Чего эта проверка НЕ делает: она не подтверждает, что набор и имена полей
// в ответах совпадают с тем, что ждёт клиент BF3. Для этого нужен живой клиент.

using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
using EVBots.Backend.Blaze;
using EVBots.Backend.Data;
using EVBots.Backend.Tdf;

namespace EVBots.Backend.Tests;

internal static class ProtocolRoundTrip
{
    public static int Run(string profilesDir, string progressDir,
        string catalogPath, string instanceDir)
    {
        Console.WriteLine("=== сквозная проверка протокола Blaze ===");
        Console.WriteLine();

        var cfg = MakeConfig(profilesDir, progressDir, catalogPath, instanceDir);
        var log = new Log(false);

        var store = new ProfileStore(cfg, log);
        store.Load();
        var accounts = new AccountsStore(
            Path.Combine(cfg.ProfilesDir, "accounts.json"), store, log);
        accounts.Load();
        var stats = new StatsCatalog(cfg.StatsCatalogPath, log);
        stats.Load();
        var catalog = new InstanceCatalog(cfg.InstanceDir, cfg.ModsDir, log);
        catalog.EnsureLayout();

        var services = new BlazeServices(cfg, store, accounts, stats, catalog, log);

        var certDir = Path.Combine(cfg.DataDir, "certs");
        using var redirectorCert =
            CertificateTool.LoadOrCreate(certDir, "evbots-redirector.pfx");
        using var blazeCert =
            CertificateTool.LoadOrCreate(certDir, "evbots-blaze.pfx");

        var redirector = new BlazeListener(cfg.RedirectorPort, redirectorCert,
            services, isRedirector: true, log);
        var blaze = new BlazeListener(cfg.BlazePort, blazeCert, services,
            isRedirector: false, log);

        redirector.Start();
        blaze.Start();

        var failures = 0;

        try
        {
            failures += RunSession(cfg, redirectorCert, "редиректор",
                cfg.RedirectorPort, "EVBotsSelftest1");
            failures += RunSession(cfg, blazeCert, "ядро Blaze",
                cfg.BlazePort, "EVBotsSelftest2");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"ОШИБКА сессии: {ex.Message}");
            failures++;
        }
        finally
        {
            redirector.Stop();
            blaze.Stop();
        }

        Console.WriteLine();
        Console.WriteLine(failures == 0
            ? "ИТОГ: протокол отвечает на все проверенные команды."
            : $"ИТОГ: провалено проверок: {failures}");
        return failures;
    }

    private static ServeConfig MakeConfig(string profilesDir, string progressDir,
        string catalogPath, string instanceDir)
    {
        // свободные порты, чтобы не мешать рабочему бэкенду
        var a = FreePort();
        var b = FreePort();

        return new ServeConfig
        {
            RedirectorPort = a,
            BlazePort = b,
            HttpPort = FreePort(),
            QosPort = FreePort(),
            GameServerAddress = "127.0.0.1:25300",
            GameServerIp = System.Net.IPAddress.Loopback,
            GameServerPort = 25300,
            ProfilesDir = profilesDir,
            ProgressDir = progressDir,
            StatsCatalogPath = catalogPath,
            InstanceDir = instanceDir,
            ModsDir = Path.Combine(instanceDir, "Admin", "Mods"),
            ServerName = "EVBots selftest",
            DataDir = Path.Combine(
                Path.GetTempPath(), "evbots-selftest-" + Guid.NewGuid().ToString("N")[..8]),
        };
    }

    private static int FreePort()
    {
        using var l = new Socket(AddressFamily.InterNetwork, SocketType.Stream,
            ProtocolType.Tcp);
        l.Bind(new System.Net.IPEndPoint(System.Net.IPAddress.Loopback, 0));
        return ((System.Net.IPEndPoint)l.LocalEndPoint!).Port;
    }

    /// <summary>Одна сессия: TLS + серия запросов с проверкой ответов.</summary>
    private static int RunSession(ServeConfig cfg, X509Certificate2 cert,
        string what, int port, string loginName)
    {
        Console.WriteLine($"--- сессия: {what} (TCP {port}) ---");

        var bad = 0;
        using var tcp = new TcpClient();
        tcp.Connect(System.Net.IPAddress.Loopback, port);
        tcp.NoDelay = true;

        using var ssl = new SslStream(tcp.GetStream(), leaveInnerStreamOpen: false);
        ssl.AuthenticateAsClient(new SslClientAuthenticationOptions
        {
            TargetHost = "gosredirector.ea.com",
            ClientCertificates = new X509CertificateCollection(),
            EnabledSslProtocols = SslProtocols.None,
            RemoteCertificateValidationCallback = (_, _, _, _) => true,
        });

        Console.WriteLine($"  TLS: {ssl.IsEncrypted}, протокол {ssl.SslProtocol}");

        // страховка от зависания: сервер обязан ответить на каждый запрос
        ssl.ReadTimeout = 8000;
        ssl.WriteTimeout = 8000;

        // 1) fetchClientConfig - клиент присылает CFID, ждём CONF со
        //    списками достижений и кодов победы
        bad += Expect(ssl, Component.Util, UtilCmd.FetchClientConfig, 1,
            new TdfNode[] { TdfNode.Str("CFID", "EVBOTS-TEST-CFID") },
            f =>
            {
                var conf = f["CONF"] as TdfMap;
                if (conf is null)
                {
                    return Fail(f, "нет карты CONF");
                }

                var keys = conf.Pairs.Select(p => p.Key.ToString()).ToList();
                if (!keys.Contains("Achievements") || !keys.Contains("WinCodes"))
                {
                    return Fail(f, "в CONF нет Achievements/WinCodes: " +
                                    string.Join(",", keys));
                }

                Console.WriteLine($"    CONF: {keys.Count} ключей, " +
                                  $"{conf.Pairs.Count} пар");
                return 0;
            },
            "fetchClientConfig");

        // 2) preAuth - запрос CDAT{TYPE,SVCN} + CINF{LOC}, ответ
        //    CIDS/CONF/INST/SVER. Без SVER или CIDS клиент не пойдёт дальше.
        bad += Expect(ssl, Component.Util, UtilCmd.PreAuth, 1,
            new TdfNode[]
            {
                TdfNode.Struct("CDAT",
                    TdfNode.Int("TYPE", 0),
                    TdfNode.Str("SVCN", "BlazeServer")),
                TdfNode.Struct("CINF", TdfNode.Int("LOC", 6)),
            },
            f =>
            {
                foreach (var need in new[] { "CIDS", "CONF", "INST", "SVER", "PLAT" })
                {
                    if (!f.Has(need))
                    {
                        return Fail(f, $"в preAuth нет поля {need}");
                    }
                }

                var sver = f.Str("SVER");
                var cids = f.List("CIDS");
                Console.WriteLine($"    SVER={sver}, CIDS={cids?.Items.Count}, " +
                                  $"INST={f.Str("INST")}, PLAT={f.Str("PLAT")}");
                return sver.StartsWith("Blaze ", StringComparison.Ordinal) ? 0 : 1;
            },
            "preAuth");

        // 3) postAuth - ответ из четырёх структур: PSS, TELE, TICK, UROP
        bad += Expect(ssl, Component.Util, UtilCmd.PostAuth, 1,
            new TdfNode[] { TdfNode.Int("CONN", 1) },
            f =>
            {
                foreach (var need in new[] { "PSS", "TELE", "TICK", "UROP" })
                {
                    if (!f.Has(need))
                    {
                        return Fail(f, $"в postAuth нет структуры {need}");
                    }
                }

                return 0;
            },
            "postAuth");

        // 4) getTelemetryServer - плоский ответ с ADRS/PORT/SKEY
        bad += Expect(ssl, Component.Util, UtilCmd.GetTelemetryServer, 1,
            new TdfNode[] { TdfNode.VarInts("FIPS", 1, 2, 3) },
            f =>
            {
                foreach (var need in new[] { "ADRS", "PORT", "SKEY", "SESS" })
                {
                    if (!f.Has(need))
                    {
                        return Fail(f, $"в getTelemetryServer нет поля {need}");
                    }
                }

                return f.Str("ADRS") == "127.0.0.1" ? 0 : 1;
            },
            "getTelemetryServer -> loopback");

        // 5) loginPersona по имени солдата - как это делает VU-клиент.
        //    Ответ: BUID, FRST, KEY, LLOG, MAIL, PDTL{DSNM,PID,STAS}, UID
        bad += Expect(ssl, Component.Authentication, AuthCmd.LoginPersona, 1,
            new TdfNode[] { TdfNode.Str("PNAM", loginName) },
            f =>
            {
                foreach (var need in new[] { "BUID", "MAIL", "PDTL", "UID" })
                {
                    if (!f.Has(need))
                    {
                        return Fail(f, $"в loginPersona нет поля {need}");
                    }
                }

                var pdtl = f.Struct("PDTL");
                if (pdtl?["DSNM"] is not TdfStr dsnm)
                {
                    return Fail(f, "в PDTL нет DSNM");
                }

                Console.WriteLine($"    BUID={f.UInt("BUID")}, " +
                                  $"MAIL={f.Str("MAIL")}, " +
                                  $"DSNM={dsnm.Value}, " +
                                  $"PID={pdtl["PID"]?.AsUInt()}");
                return 0;
            },
            $"loginPersona '{loginName}'");

        // 6) getStatGroup — отдаём блок statgroups как есть (>64 КБ,
        //    значит кадр с расширенным размером)
        bad += ExpectRaw(ssl, Component.Stats, StatsCmd.GetStatGroup, 1,
            new[] { TdfNode.Str("NAME", "player_mpdefault2") },
            payload =>
            {
                Console.WriteLine($"    ответ {payload.Length:N0} байт, " +
                                  $"начало {Convert.ToHexString(payload, 0, 8)}");
                return payload.Length > 100_000 ? 0 : 1;
            },
            "getStatGroup (сырой блок >64K, расширенный размер)");

        // 7) getStatsByGroupAsync — значения прогресса
        bad += Expect(ssl, Component.Stats, StatsCmd.GetStatsByGroupAsync, 1,
            new TdfNode[] { TdfNode.Str("NAME", "player_mpdefault2") },
            f =>
            {
                Console.WriteLine($"    STATS: {f.Str("SNAM")}, " +
                                  $"EID={f.UInt("EID")}");
                return 0;
            },
            "getStatsByGroupAsync");

        // 8) getServerInstance — редиректор отдаёт адрес игрового сервера
        bad += Expect(ssl, Component.Redirector, RedirectorCmd.GetServerInstance, 1,
            new TdfNode[] { TdfNode.Int("CLNT", 3) },
            f =>
            {
                if (f["ADDR"] is not TdfUnion addr)
                {
                    return Fail(f, "в ответе редиректора нет union ADDR");
                }

                var valu = addr.Data as TdfStruct;
                if (valu?["HOST"] is not TdfStr host)
                {
                    return Fail(f, "в ADDR нет VALU.HOST");
                }

                var port = valu["PORT"]?.AsUInt() ?? 0;
                Console.WriteLine($"    ADDR член={addr.ActiveMember}, " +
                                  $"HOST={host.Value}, PORT={port}, " +
                                  $"SECU={f.UInt("SECU")}");
                return port > 0 ? 0 : 1;
            },
            "getServerInstance");

        // 9) userSettingsSave + LoadAll
        bad += Expect(ssl, Component.Util, UtilCmd.UserSettingsSave, 1,
            new TdfNode[]
            {
                TdfNode.Int("PID", 234471775),
                TdfNode.Str("KEY", "cust"),
                TdfNode.Bin("DATA", new byte[] { 1, 2, 3, 4, 5 }),
            },
            _ => 0, "userSettingsSave");

        bad += Expect(ssl, Component.Util, UtilCmd.UserSettingsLoadAll, 1,
            new TdfNode[] { TdfNode.Int("PID", 234471775) },
            f => f["SMAP"] is TdfMap
                ? 0
                : Fail(f, "в userSettingsLoadAll нет карты SMAP"),
            "userSettingsLoadAll -> SMAP");

        // 10) список прав
        bad += Expect(ssl, Component.Authentication, AuthCmd.ListUserEntitlements2, 1,
            new[] { TdfNode.Str("ETAG", "") },
            _ => 0, "listUserEntitlements2");

        // 11) неизвестная команда: сервер обязан ответить ошибкой, иначе клиент зависнет
        bad += Expect(ssl, Component.Util, 0x7F, 1,
            new[] { TdfNode.Int("TEST", 1) },
            _ => 0, "неизвестная команда (ждём ошибку, не тишину)");

        // после всего — сервис жив?
        bad += Expect(ssl, Component.Util, UtilCmd.Ping, 1,
            new[] { TdfNode.Int("TCL", 0) },
            f => f.Has("STIM") ? 0 : Fail(f, "ping без поля STIM"),
            "ping после серии запросов");

        return bad;
    }

    private static int Fail(TdfFields f, string why)
    {
        Console.WriteLine($"    ПРОВАЛ: {why}; получено {f.Count} полей: " +
                          $"[{string.Join(", ", f.All.Select(x => x.Label))}]");
        return 1;
    }

    /// <summary>
    /// Читает кадры, пока не найдёт ответ на наш запрос.
    ///
    /// Сервер шлёт уведомления (UserAdded, UserUpdated, Stats 0x32) вперемешку
    /// с ответами, поэтому их надо пропускать — так же, как это делает
    /// настоящий клиент Blaze.
    /// </summary>
    private static (ReadFrameResult? Frame, int Skipped) ReadReply(
        SslStream ssl, ushort comp, ushort cmd)
    {
        var skipped = 0;

        for (var i = 0; i < 16; i++)
        {
            var f = ReadFrame(ssl);
            if (f is null)
            {
                return (null, skipped);
            }

            if (f.ComponentId == comp && f.CommandId == cmd)
            {
                return (f, skipped);
            }

            skipped++;
        }

        return (null, skipped);
    }

    /// <summary>Отправляет запрос, читает ответ, разбирает TDF, зовёт проверку.</summary>
    private static int Expect(SslStream ssl, ushort comp, ushort cmd, int messageId,
        TdfNode[] request, Func<TdfFields, int> check, string label)
    {
        var frame = BlazeFrame.Build(comp, cmd, 0, BlazeMessageType.Request,
            messageId, TdfCodec.Encode(request));

        try
        {
            ssl.Write(frame, 0, frame.Length);
            ssl.Flush();

            var (reply, skipped) = ReadReply(ssl, comp, cmd);

            if (reply is null)
            {
                Console.WriteLine($"  {label}: ОТВЕТ НЕ ПРИШЁЛ");
                return 1;
            }

            if (reply.ComponentId != comp || reply.CommandId != cmd)
            {
                Console.WriteLine($"  {label}: не дождались ответа, " +
                                  $"последний кадр comp={reply.ComponentId} " +
                                  $"cmd={reply.CommandId}" +
                                  (skipped > 0 ? $" (пропущено {skipped})" : ""));
                return 1;
            }

            TdfFields fields;
            try
            {
                fields = TdfCodec.Decode(reply.Payload);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"  {label}: payload не разобран: {ex.Message}");
                return 1;
            }

            var r = check(fields);
            Console.WriteLine($"  {label}: " + (r == 0 ? "OK" : "провал") +
                              (skipped > 0 ? $" (уведомлений пропущено: {skipped})" : ""));
            return r;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  {label}: ошибка связи: {ex.Message}");
            return 1;
        }
    }

    /// <summary>Как Expect, но отдаёт сырой payload без разбора TDF.</summary>
    private static int ExpectRaw(SslStream ssl, ushort comp, ushort cmd,
        int messageId, TdfNode[] request, Func<byte[], int> check, string label)
    {
        var frame = BlazeFrame.Build(comp, cmd, 0, BlazeMessageType.Request,
            messageId, TdfCodec.Encode(request));

        try
        {
            ssl.Write(frame, 0, frame.Length);
            ssl.Flush();

            var (reply, skippedRaw) = ReadReply(ssl, comp, cmd);
            if (reply is null)
            {
                Console.WriteLine($"  {label}: ОТВЕТ НЕ ПРИШЁЛ");
                return 1;
            }

            var r = check(reply.Payload);
            Console.WriteLine($"  {label}: " + (r == 0 ? "OK" : "провал") +
                              (skippedRaw > 0 ? $" (уведомлений: {skippedRaw})" : ""));
            return r;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  {label}: ошибка связи: {ex.Message}");
            return 1;
        }
    }

    /// <summary>Кадр, прочитанный из сокета.</summary>
    private sealed record ReadFrameResult(ushort ComponentId, ushort CommandId,
        byte[] Payload);

    private static ReadFrameResult? ReadFrame(SslStream ssl)
    {
        var header = ReadExact(ssl, BlazeFrame.HeaderSize);
        if (header is null)
        {
            return null;
        }

        // при расширенном размере после заголовка идут 2 байта старшей
        // половины длины (BIG-endian, как в эталоне FireFrame: header[12]<<24
        // | header[13]<<16), и только потом payload
        var extendedHigh = -1;
        if (BlazeFrame.HasExtendedSize(header))
        {
            var ext = ReadExact(ssl, 2);
            if (ext is null)
            {
                return null;
            }

            extendedHigh = (ext[0] << 8) | ext[1];
        }

        var h = BlazeFrame.ParseHeader(header, extendedHigh);

        var payload = h.PayloadLength > 0 ? ReadExact(ssl, h.PayloadLength) : [];
        if (payload is null)
        {
            return null;
        }

        return new ReadFrameResult((ushort)h.ComponentId, (ushort)h.CommandId, payload);
    }

    private static byte[]? ReadExact(Stream s, int count)
    {
        if (count <= 0)
        {
            return [];
        }

        var buf = new byte[count];
        var got = 0;
        while (got < count)
        {
            var n = s.Read(buf, got, count - got);
            if (n == 0)
            {
                return null;
            }

            got += n;
        }

        return buf;
    }
}