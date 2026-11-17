// Program.cs — точка входа EVBots.Backend.
//
// Режимы:
//   tdf-selftest [--catalog <файл>]   проверка кодека на эталонном payload
//   serve [опции]                     основной сервис (фаза 2)
//   gen-cert <каталог>                сгенерировать сертификаты для TLS
//   --help / -h                       справка
//
// Опции serve (порты и пути совпадают со схемой BF3NativeBots, чтобы гостю
// не пришлось ничего запоминать):
//   --redirector-port N        редиректор Blaze (по умолчанию 42127)
//   --blaze-port N             ядро Blaze (по умолчанию 10041)
//   --http-port N              HTTP: список серверов, статистика (8080)
//   --qos-port N               QoS-пробы игроков (17554)
//   --game-server-address IP:PORT  адрес игрового сервера (по умолчанию авто: LAN-IP:25300)
//   --profiles-dir PATH        каталог профилей (по умолчанию ./profiles)
//   --progress-dir PATH        каталог прогресса (по умолчанию ./progress)
//   --stats-catalog FILE       каталог статов (по умолчанию <progress-dir>/bf3-stats-catalog.json)
//   --advertise-ip IP          IP для серверного браузера (по умолчанию авто)
//   --server-name "Имя"        имя сервера для браузера
//   --server-id GUID           свой идентификатор сервера (вместо server.key)
//   --debug                    подробный лог

using EVBots.Backend.Tests;

namespace EVBots.Backend;

internal static class Program
{
    public static int Main(string[] args)
    {
        Console.OutputEncoding = System.Text.Encoding.UTF8;
        Console.WriteLine("EV Backend - local Blaze emulator for BF3 (Zeus analogue in VU)");
        Console.WriteLine();

        if (args.Length == 0)
        {
            PrintHelp();
            return 0;
        }

        var cmd = args[0].ToLowerInvariant();
        var rest = args[1..];

        switch (cmd)
        {
            case "--help" or "-h" or "help" or "?":
                PrintHelp();
                return 0;

            case "tdf-selftest":
            {
                var catalog = GetOption(rest, "--catalog")
                              ?? DefaultCatalogPath();
                return TdfRoundTrip.Run(catalog);
            }

            case "protocol-selftest":
            {
                var profiles = GetOption(rest, "--profiles-dir")
                               ?? Path.Combine(Environment.CurrentDirectory,
                                   "profiles");
                var progress = GetOption(rest, "--progress-dir")
                               ?? Path.Combine(Environment.CurrentDirectory,
                                   "progress");
                var catalog = GetOption(rest, "--catalog")
                              ?? Path.Combine(progress, "bf3-stats-catalog.json");
                var instance = GetOption(rest, "--instance")
                               ?? Path.Combine(
                                   Environment.GetFolderPath(
                                       Environment.SpecialFolder.ApplicationData),
                                   "EVBots", "server");

                return ProtocolRoundTrip.Run(profiles, progress, catalog, instance);
            }

            case "ev-selftest":
                return EvRoundTrip.Run();

            case "progress-selftest":
            {
                var root = GetOption(rest, "--dir") ?? Path.Combine(
                    Environment.CurrentDirectory, "progress-selftest-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff"));
                return ProgressRoundTrip.Run(root);
            }

            case "report-selftest":
            {
                // 18.10: разбор журналов отчётов матча (*.rpt старого бэкенда: 20 байт заголовка BLRJ + payload)
                foreach (var f in rest)
                {
                    var data = File.ReadAllBytes(f);
                    var off = data.Length > 20 && data[0] == (byte)'B' && data[1] == (byte)'L' ? 20 : 0;
                    var players = Blaze.ReportReader.ReadPlayers(data[off..], out var fin);
                    foreach (var (id, st) in players)
                    {
                        st.TryGetValue("c___k_g", out var k);
                        st.TryGetValue("sc_general", out var g);
                        Console.WriteLine($"{Path.GetFileName(f)}: FNSH={fin} persona={id} стат={st.Count} убийств={k} sc_general={g}");
                    }
                }

                return 0;
            }

            case "gen-cert":
                return CertificateTool.Run(rest.FirstOrDefault() ?? "certs");

            case "serve":
                return ServeCommand.Run(rest);

            default:
                Console.Error.WriteLine($"Неизвестная команда: {cmd}");
                Console.WriteLine();
                PrintHelp();
                return 2;
        }
    }

    public static string? GetOption(string[] args, string name)
    {
        for (var i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
            {
                return args[i + 1];
            }
        }

        return null;
    }

    public static bool HasFlag(string[] args, string name) =>
        args.Any(a => string.Equals(a, name, StringComparison.OrdinalIgnoreCase));

    private static string DefaultCatalogPath() =>
        Path.Combine(Environment.CurrentDirectory, "progress", "bf3-stats-catalog.json");

    private static void PrintHelp()
    {
        Console.WriteLine("Использование:");
        Console.WriteLine("  EV Backend serve [опции]      запустить сервис");
        Console.WriteLine("  EV Backend gen-cert <каталог> сгенерировать сертификаты");
        Console.WriteLine("  EV Backend tdf-selftest      проверить TDF-кодек");
        Console.WriteLine("  EV Backend protocol-selftest сквозная проверка протокола");
        Console.WriteLine("  EV Backend ev-selftest       проверка EV-канала (Zeus-функции, TCP 7828)");
        Console.WriteLine("  EV Backend progress-selftest --dir PATH  проверка сохранения прогресса на временных профилях");
        Console.WriteLine("  EV Backend --help            эта справка");
        Console.WriteLine();
        Console.WriteLine("Опции serve:");
        Console.WriteLine("  --redirector-port N        редиректор Blaze           (42127)");
        Console.WriteLine("  --blaze-port N             ядро Blaze                (10041)");
        Console.WriteLine("  --http-port N              HTTP: серверы, статистика  (8080)");
        Console.WriteLine("  --qos-port N               QoS-пробы игроков         (17554)");
        Console.WriteLine("  --game-server-address A:B  адрес игрового сервера  (авто: LAN-IP:25300)");
        Console.WriteLine("  --profiles-dir PATH        каталог профилей           (./profiles)");
        Console.WriteLine("  --progress-dir PATH        каталог прогресса          (./progress)");
        Console.WriteLine("  --stats-catalog FILE       каталог статов (лестница рангов)");
        Console.WriteLine("  --instance PATH            инстанс сервера с Admin\\ (из server.config)");
        Console.WriteLine("  --mods-dir PATH            папка модов Admin\\Mods");
        Console.WriteLine("  --advertise-ip IP          IP для серверного браузера (авто)");
        Console.WriteLine("  --server-name \"Имя\"       имя сервера в браузере");
        Console.WriteLine("  --server-id GUID          свой ID сервера (вместо server.key)");
        Console.WriteLine("  --unlisted                 не показывать сервер в списке (вход по адресу/id)");
        Console.WriteLine("  --frequency 30|60|120      частота движка сервера (в /servers для клиентов, как у VU)");
        Console.WriteLine("  --no-join-user-to-server   не слать серверу UserAdded о входящем игроке (как до 20.10)");
        Console.WriteLine("  --gos-achievements         отдавать коды достижений EA (сервер пойдёт в интернет)");
        Console.WriteLine("  --no-editorial             не отдавать серверу server.xml на 127.0.0.1:80");
        Console.WriteLine("  --editorial-file FILE      свой server.xml (иначе BF-Portable\\htdocs\\easo\\...)");
        Console.WriteLine("  --debug                    подробный лог");
        Console.WriteLine();
        Console.WriteLine("Интернет не требуется: аккаунты, солдаты, прогресс и реестр");
        Console.WriteLine("серверов живут локально (это и заменяет Zeus в VU).");
    }
}
