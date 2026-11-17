// BlazeTls.cs — TLS-рукопожатие с клиентом BF3 (dirtysock/DICE).
//
// Живая проверка 09.10.2026 показала: клиент (и выделенный сервер) на
// редиректор и ядро Blaze шлёт ТОЛЬКО SSL 3.0 ClientHello с RC4-шифрами
// (0x0005 RC4-SHA, 0x0004 RC4-MD5). Структуру DN сертификата клиент
// проверяет: принимает только лист EA (issuer «OTG3 Certificate
// Authority») — см. Otg3Cert.cs; подпись не проверяет.
//
// Schannel (System.Net.Security.SslStream) на современной Windows SSL3/RC4
// не умеет — отсюда «TLS не сошёлся». Рабочий бэкенд потому и тащит
// BouncyCastle, причём именно СТАРЫЙ API (Org.BouncyCastle.Crypto.Tls):
// в новом API (Org.BouncyCastle.Tls) RC4 отсутствует вовсе.
//
// Снимать эти шифры нельзя: клиент умеет только их.

using System.Security.Cryptography.X509Certificates;
using Org.BouncyCastle.Crypto;
using Org.BouncyCastle.Crypto.Tls;
using Org.BouncyCastle.Security;

namespace EVBots.Backend.Blaze;

internal static class BlazeTls
{
    /// <summary>
    /// Принять TLS-соединение на потоке и вернуть поток прикладных данных.
    /// Блокирующая операция — вызывать из отдельной задачи.
    /// trace — временная трассировка сырых байтов рукопожатия.
    /// </summary>
    public static Stream Accept(Stream transport, X509Certificate2 certificate,
        Action<string>? trace = null)
    {
        using var rsa = certificate.GetRSAPrivateKey()
            ?? throw new InvalidOperationException(
                "сертификат Blaze должен быть RSA");

        var key = PrivateKeyFactory.CreateKey(rsa.ExportPkcs8PrivateKey());

        // DER сертификата -> структура ASN.1 -> цепочка TLS
        var structure = Org.BouncyCastle.Asn1.X509.X509CertificateStructure
            .GetInstance(Org.BouncyCastle.Asn1.Asn1Object
                .FromByteArray(certificate.RawData));
        var chain = new Certificate([structure]);

        var wire = trace is null ? transport : new HandshakeTrace(transport, trace);
        var protocol = new TlsServerProtocol(wire, new SecureRandom());
        protocol.Accept(new EVBotsTlsServer(chain, key));
        return protocol.Stream;
    }

    /// <summary>ВРЕМЕННО: дамп байтов рукопожатия в обе стороны.</summary>
    private sealed class HandshakeTrace(Stream inner, Action<string> log) : Stream
    {
        public override int Read(byte[] buffer, int offset, int count)
        {
            var n = inner.Read(buffer, offset, count);
            if (n > 0)
            {
                log($"TLS-рукопожатие ← {n} Б: " +
                    Convert.ToHexString(buffer, offset, n));
            }
            return n;
        }

        public override void Write(byte[] buffer, int offset, int count)
        {
            log($"TLS-рукопожатие → {count} Б: " +
                Convert.ToHexString(buffer, offset, count));
            inner.Write(buffer, offset, count);
        }

        public override bool CanRead => inner.CanRead;
        public override bool CanSeek => false;
        public override bool CanWrite => inner.CanWrite;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException();
            set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
    }

    /// <summary>
    /// Сервер с протоколами и шифрами ровно такими, какие понимает
    /// dirtysock BF3: вниз до SSL 3.0, RC4 в первых строках.
    /// </summary>
    private sealed class EVBotsTlsServer(Certificate certificate,
        AsymmetricKeyParameter key) : DefaultTlsServer
    {
        protected override ProtocolVersion MinimumVersion => ProtocolVersion.SSLv3;

        protected override ProtocolVersion MaximumVersion => ProtocolVersion.TLSv12;

        // BF3 (SSL 3.0) не сигнализирует RFC 5746 secure renegotiation. Дефолт
        // AbstractTlsPeer из-за этого рвёт рукопожатие — не рвём. Версию ответа
        // базовый GetServerVersion() возьмёт из Minimum/MaximumVersion сам.
        public override void NotifySecureRenegotiation(bool secureRenegotiation)
        {
        }

        protected override int[] GetCipherSuites() =>
        [
            CipherSuite.TLS_RSA_WITH_RC4_128_SHA,   // 0x0005 — то, что шлёт BF3
            CipherSuite.TLS_RSA_WITH_RC4_128_MD5,   // 0x0004
            CipherSuite.TLS_RSA_WITH_AES_128_CBC_SHA,
            CipherSuite.TLS_RSA_WITH_AES_256_CBC_SHA,
            CipherSuite.TLS_RSA_WITH_3DES_EDE_CBC_SHA,
        ];

        protected override TlsEncryptionCredentials GetRsaEncryptionCredentials() =>
            new DefaultTlsEncryptionCredentials(mContext, certificate, key);

        protected override TlsSignerCredentials GetRsaSignerCredentials() =>
            new DefaultTlsSignerCredentials(mContext, certificate, key);
    }
}
