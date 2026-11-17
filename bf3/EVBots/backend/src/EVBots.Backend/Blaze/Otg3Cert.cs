// Otg3Cert.cs — лист-сертификат в структуре настоящего EA-редиректора.
//
// Живая проверка 09.10.2026: клиент dirtysock (bf3.exe) рвёт TLS-рукопожатие
// сразу после Certificate, если лист не той структуры. Эталон снят сырым
// дампом рукопожатия с рабочего бэкенда: issuer «OTG3 Certificate
// Authority», subject gosredirector.ea.com (DN самой EA), RSA 1024,
// SHA1withRSA, serial 1, даты 2012–2039, БЕЗ расширений.
//
// Рабочий бэкенд генерирует такой лист на КАЖДЫЙ старт и ставит один и
// тот же сертификат и на редиректор, и на ядро Blaze — значит, подпись
// клиент не проверяет. Делаем так же: свежий ключ на каждый старт,
// DN-структуры и сроки — байт-в-байт из эталона.
//
// pfx «Root Agency» (раньше лежал в %APPDATA%\EVBots\certs) клиент
// отвергает — см. MODLOG, раздел 8.

using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Org.BouncyCastle.Asn1;
using Org.BouncyCastle.Asn1.X509;
using Org.BouncyCastle.Crypto.Generators;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.Math;
using Org.BouncyCastle.Pkcs;
using Org.BouncyCastle.Security;
using Org.BouncyCastle.X509;

namespace EVBots.Backend.Blaze;

internal static class Otg3Cert
{
    /// <summary>
    /// Эталон листа, снятый с рабочего бэкенда 09.10.2026 (666 Б, открытый
    /// текст SSL3-рукопожатия). Из него берём серийник, алгоритм, DN-структуры
    /// и сроки байт-в-байт; открытый ключ и подпись — свои.
    /// </summary>
    private const string TemplateDerHex =
        "30820296308201FFA003020102020101300D06092A864886F70D010104050030" +
        "81A0310B30090603550406130255533113301106035504080C0A43616C69666F" +
        "726E69613115301306035504070C0C526564776F6F642043697479311E301C06" +
        "0355040A0C15456C656374726F6E696320417274732C20496E632E3120301E06" +
        "0355040B0C174F6E6C696E6520546563686E6F6C6F67792047726F7570312330" +
        "2106035504030C1A4F54473320436572746966696361746520417574686F7269" +
        "7479301E170D3132303130313030303030305A170D3339303531393030303030" +
        "305A308180310B30090603550406130255533113301106035504080C0A43616C" +
        "69666F726E6961311E301C060355040A0C15456C656374726F6E696320417274" +
        "732C20496E632E311D301B060355040B0C14476C6F62616C204F6E6C696E6520" +
        "53747564696F311D301B06035504030C14676F7372656469726563746F722E65" +
        "612E636F6D30819F300D06092A864886F70D010101050003818D003081890281" +
        "81009A0BC5912CA3E14F6D5104600B44C3576F6DCFE13CD4541D69E8942FA04A" +
        "E9CED1CB6077C59A7B3714F22AA18E9A16CA5AF1E0E74595ECC98EC543F3466F" +
        "D8D62DAE5D231D6D804C5D0784ED56748910CFA277427F906BA1EB615C4BF293" +
        "ED5243F57CE17CD343B1663AD2882C31CE855FBAF41117B34C3E020A1A0D462B" +
        "5F7F0203010001300D06092A864886F70D0101010500038181000D3F72AFB472" +
        "BA4A64F88FBFCC4A3A9E8DDAD436DF438655B2397B56D0FB58776743882EB185" +
        "C4CED7686DCF2FDDEA6B42DE2FA9E226BBFE0D1CFD8086B96E9A2C2D96DCA63B" +
        "CE72F8D0D8693B27D07883F4D79A71D105B174533C2DF64308F2702C074E1156" +
        "1E08B19B854F6B76729B470720498FF7F1D3CE68BA19C5C73EF8";

    /// <summary>Сгенерировать лист в эталонной структуре OTG3.</summary>
    public static X509Certificate2 Generate(Log log)
    {
        var outer = (Asn1Sequence)Asn1Object.FromByteArray(
            Convert.FromHexString(TemplateDerHex));
        var template = (Asn1Sequence)outer[0];          // TBSCertificate

        // RSA 1024 — как в эталоне
        var kpg = GeneratorUtilities.GetKeyPairGenerator("RSA");
        kpg.Init(new RsaKeyGenerationParameters(
            BigInteger.ValueOf(65537), new SecureRandom(), 1024, 8));
        var kp = kpg.GenerateKeyPair();
        var spki = SubjectPublicKeyInfoFactory.CreateSubjectPublicKeyInfo(
            kp.Public);

        // TBS заново: всё из шаблона байт-в-байт, кроме открытого ключа
        var items = template.ToArray();
        var rebuilt = new Asn1Encodable[items.Length];
        Array.Copy(items, rebuilt, items.Length);
        rebuilt[6] = spki;                              // [6] — SubjectPublicKeyInfo
        var tbs = new DerSequence(rebuilt);

        var signer = SignerUtilities.GetSigner("SHA-1withRSA");
        signer.Init(true, kp.Private);
        var tbsBytes = tbs.GetDerEncoded();
        signer.BlockUpdate(tbsBytes, 0, tbsBytes.Length);
        var signature = signer.GenerateSignature();

        var cert = Org.BouncyCastle.Asn1.X509.X509CertificateStructure
            .GetInstance(new DerSequence(
                tbs,
                outer[1],                       // sig-alg sha1WithRSA из эталона
                new DerBitString(signature)));

        // в X509Certificate2 с приватным ключом (через PKCS#12 в памяти)
        var store = new Pkcs12StoreBuilder().Build();
        store.SetKeyEntry("otg3",
            new AsymmetricKeyEntry(kp.Private),
            new[] { new X509CertificateEntry(
                new Org.BouncyCastle.X509.X509Certificate(cert)) });
        using var ms = new MemoryStream();
        store.Save(ms, "123456".ToCharArray(), new SecureRandom());
        var leaf = X509CertificateLoader.LoadPkcs12(
            ms.ToArray(), "123456", X509KeyStorageFlags.Exportable);

        log.Info($"  TLS: generated OTG3 leaf " +
                 $"(DER {cert.GetDerEncoded().Length} Б, эталон 666 Б)");
        return leaf;
    }
}
