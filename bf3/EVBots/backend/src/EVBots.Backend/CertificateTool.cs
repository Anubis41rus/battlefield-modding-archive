// CertificateTool.cs — генерация самоподписанных сертификатов для TLS.
//
// Зачем: клиент BF3 подключается по TLS и проверяет имя хоста. Нам нужно
// выдать сертификат, которому он поверит, поэтому мы делаем собственный
// корневой CA и подписываем им сертификаты редиректора и Blaze.
// Названия хостов — те, что клиент запрашивает у редиректора EA
// (напр. gosredirector.ea.com), плюс локальные имена.
//
// Формат: PFX (PKCS#12), как в BF-Portable\Redirector\*.pfx — тот же
// формат, который уже проверен на практике.

using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace EVBots.Backend;

internal static class CertificateTool
{
    public const string CaFileName = "evbots-ca.pfx";
    public const string CaPassword = "evbots";

    public static int Run(string outputDir)
    {
        Directory.CreateDirectory(outputDir);

        Console.WriteLine($"Генерация сертификатов в {outputDir}");

        using var caKey = RSA.Create(2048);
        var caReq = new CertificateRequest(
            "CN=EVBots Local CA, O=EVBots, OU=EVBots Backend",
            caKey, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        caReq.CertificateExtensions.Add(
            new X509BasicConstraintsExtension(true, false, 0, true));
        caReq.CertificateExtensions.Add(
            new X509KeyUsageExtension(
                X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.CrlSign |
                X509KeyUsageFlags.DigitalSignature, true));
        caReq.CertificateExtensions.Add(
            new X509SubjectKeyIdentifierExtension(caReq.PublicKey, false));

        var now = DateTimeOffset.UtcNow;
        using var caCert = caReq.CreateSelfSigned(now.AddDays(-1), now.AddYears(5));
        var caPath = Path.Combine(outputDir, CaFileName);
        File.WriteAllBytes(caPath, caCert.Export(X509ContentType.Pfx, CaPassword));
        Console.WriteLine($"  {CaFileName}");

        // имена, которые запрашивает клиент
        var hostNames = new[]
        {
            "gosredirector.ea.com",
            "spring14.gosredirector.ea.com",
            "373243-gosprapp356.ea.com",
            "373244-gosprapp357.ea.com",
            "localhost",
        };

        foreach (var (file, label) in new[]
                 {
                     ("evbots-redirector.pfx", "редиректор"),
                     ("evbots-blaze.pfx", "ядро Blaze"),
                 })
        {
            var path = Path.Combine(outputDir, file);
            CreateLeaf(outputDir, caCert, file, label, hostNames, now);
            Console.WriteLine($"  {file} ({label})");
        }

        Console.WriteLine();
        Console.WriteLine("Готово. Пароль PFX: " + CaPassword);
        return 0;
    }

    private static void CreateLeaf(string dir, X509Certificate2 caCert, string fileName,
        string label, string[] hostNames, DateTimeOffset now)
    {
        using var key = RSA.Create(2048);
        var req = new CertificateRequest(
            $"CN={hostNames[0]}, O=EVBots, OU={label}",
            key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        req.CertificateExtensions.Add(
            new X509BasicConstraintsExtension(false, false, 0, true));
        req.CertificateExtensions.Add(
            new X509KeyUsageExtension(
                X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment,
                true));
        req.CertificateExtensions.Add(
            new X509EnhancedKeyUsageExtension(
                new OidCollection { new("1.3.6.1.5.5.7.3.1") }, false));
        var san = new SubjectAlternativeNameBuilder();
        foreach (var h in hostNames)
        {
            san.AddDnsName(h);
        }

        foreach (var ip in LocalAddresses())
        {
            if (IPAddress.TryParse(ip, out var addr))
            {
                san.AddIpAddress(addr);
            }
        }

        req.CertificateExtensions.Add(san.Build());

        var serial = new byte[16];
        RandomNumberGenerator.Fill(serial);
        serial[0] &= 0x7F; // положительный серийник

        using var cert = req.Create(caCert, now.AddDays(-1), now.AddYears(4), serial);
        using var withKey = cert.CopyWithPrivateKey(key);

        var path = Path.Combine(dir, fileName);
        File.WriteAllBytes(path, withKey.Export(X509ContentType.Pfx, CaPassword));
    }

    /// <summary>
    /// Загружает сертификат для SslStream. Если файла нет — создаёт на лету
    /// в памяти, чтобы сервис поднимался даже без предварительной подготовки.
    /// </summary>
    public static X509Certificate2 LoadOrCreate(string certDir, string fileName)
    {
        var path = Path.Combine(certDir, fileName);

        if (File.Exists(path))
        {
            try
            {
                return Load(path);
            }
            catch (CryptographicException)
            {
                // повреждённый файл — перегенерируем
            }
        }

        Directory.CreateDirectory(certDir);
        Run(certDir);

        return Load(path);
    }

    /// <summary>
    /// Загрузка PFX. Конструктор X509Certificate2 устарел (.NET 9+),
    /// поэтому используем X509CertificateLoader (проект под net10.0).
    /// </summary>
    private static X509Certificate2 Load(string path) =>
        X509CertificateLoader.LoadPkcs12FromFile(path, CaPassword,
            X509KeyStorageFlags.Exportable);

    /// <summary>Локальные IP этого компьютера - для SAN сертификата.</summary>
    public static string[] LocalAddresses()
    {
        var list = new List<string> { "127.0.0.1" };
        try
        {
            foreach (var ip in System.Net.Dns.GetHostAddresses(
                         System.Net.Dns.GetHostName()))
            {
                if (ip.AddressFamily ==
                    System.Net.Sockets.AddressFamily.InterNetwork)
                {
                    list.Add(ip.ToString());
                }
            }
        }
        catch
        {
            // определить не удалось - останется только loopback
        }

        return list.Distinct().ToArray();
    }
}
