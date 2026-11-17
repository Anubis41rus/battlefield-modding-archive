// BlazeListener.cs — приём соединений Blaze (TLS) и разбор кадров.
//
// Кадр Blaze (little-endian):
//   0..1   размер payload (uint16)
//   2..3   component ID (uint16)
//   4..5   command ID (uint16)
//   6..7   error code (uint16)
//   8      старшие 4 бита — тип сообщения (0=запрос, 1=ответ, 2=уведомление, 3=ошибка)
//   9      бит 4 — «есть расширенный размер payload»
//   10..11 message ID (24 бита)
//   12..   payload (TDF)
//
// TLS обязателен: клиент BF3 подключается по HTTPS-подобному каналу и
// проверяет имя хоста, поэтому мы отдаём собственный сертификат.

using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography.X509Certificates;

namespace EVBots.Backend.Blaze;

internal sealed class BlazeListener(
    int port,
    X509Certificate2 certificate,
    BlazeServices services,
    bool isRedirector,
    Log log)
{
    private readonly CancellationTokenSource _stop = new();
    private Socket? _socket;
    private Task? _acceptLoop;

    public int Port => port;
    public string Kind => isRedirector ? "redirector" : "Blaze";

    public void Start()
    {
        _socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream,
            ProtocolType.Tcp);
        _socket.SetSocketOption(SocketOptionLevel.Socket,
            SocketOptionName.ReuseAddress, true);
        _socket.Bind(new IPEndPoint(IPAddress.Any, port));
        _socket.Listen(64);

        _acceptLoop = Task.Run(() => AcceptLoopAsync(_stop.Token));
        log.Ok($"{Kind}: listening on TCP {port}");
    }

    public void Stop()
    {
        try
        {
            _stop.Cancel();
            _socket?.Close();
        }
        catch
        {
            // игнорируем: закрытие при остановке
        }
    }

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            Socket client;
            try
            {
                client = await _socket!.AcceptAsync(ct);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (Exception ex)
            {
                if (!ct.IsCancellationRequested)
                {
                    log.Warn($"{Kind}: ошибка приёма: {ex.Message}");
                }

                continue;
            }

            _ = Task.Run(() => HandleAsync(client, ct), ct);
        }
    }

    private async Task HandleAsync(Socket socket, CancellationToken ct)
    {
        var endpoint = (IPEndPoint)socket.RemoteEndPoint!;
        var session = new BlazeSession(services, isRedirector,
            endpoint.Address, log);

        try
        {
            socket.NoDelay = true;

            await using var network = new NetworkStream(socket, ownsSocket: false);

            // Рукопожатие делает BouncyCastle (SSL3 + RC4) — Schannel это уже
            // не умеет, см. BlazeTls.cs. Рукопожатие блокирующее, уводим его
            // с потока обработки.
            Stream stream;
            try
            {
                stream = await Task.Run(
                    () => BlazeTls.Accept(network, certificate,
                        m => log.Debug($"{Kind} #{session.Id}: {m}")), ct);
            }
            catch (Exception ex)
            {
                log.Warn($"{Kind}: TLS не сошёлся с " +
                         $"{endpoint.Address}:{endpoint.Port} — " +
                         $"{ex.GetType().Name}: {ex.Message}");
                log.Debug(ex.ToString());
                throw;
            }

            await using var tls = stream;
            session.AttachStream(new SslStreamHolder(tls));

            log.Info($"{Kind}: connection {session.Id} ← " +
                     $"{endpoint.Address}:{endpoint.Port}");

            services.Register(session);

            await PumpAsync(session, stream, ct);
        }
        catch (OperationCanceledException)
        {
            // штатно
        }
        catch (Exception ex)
        {
            // Info (было Debug, 13.10): исключение в цикле чтения молча убивало
            // сессию сервера — после этого он видел только RPC_TIMEOUT.
            log.Info($"{Kind}: connection {session.Id} broken: " +
                     $"{ex.GetType().Name}: {ex.Message}");
            log.Debug(ex.ToString());
        }
        finally
        {
            services.Unregister(session);
            session.Dispose();
            try
            {
                socket.Dispose();
            }
            catch
            {
                // игнорируем
            }
        }
    }

    /// <summary>Цикл чтения кадров. Кадры могут приходить пачками.</summary>
    private static async Task PumpAsync(BlazeSession session, Stream stream,
        CancellationToken ct)
    {
        session.Log.Debug($"сессия {session.Id}: старт приёма кадров");

        var header = new byte[BlazeFrame.HeaderSize];
        var headerGot = 0;

        while (!ct.IsCancellationRequested)
        {
            // 1) заголовок — ровно 12 байт
            headerGot = 0;
            while (headerGot < header.Length)
            {
                var n = await stream.ReadAsync(header.AsMemory(headerGot,
                    header.Length - headerGot), ct);
                if (n == 0)
                {
                    session.Log.Info(
                        $"сессия {session.Id}: поток закрыт (прочитано {headerGot})");
                    return; // соединение закрыто клиентом
                }

                headerGot += n;
            }

            session.Log.Debug(
                $"сессия {session.Id}: заголовок {Convert.ToHexString(header)}");

            // 1б) если размер расширенный, читаем ещё 2 байта старшей половины
            var extendedHigh = -1;
            var extBytes = BlazeFrame.ExtendedSizeBytes(header);
            if (extBytes > 0)
            {
                var ext = new byte[extBytes];
                var extGot = 0;
                while (extGot < ext.Length)
                {
                    var n = await stream.ReadAsync(ext.AsMemory(extGot,
                        ext.Length - extGot), ct);
                    if (n == 0)
                    {
                        return;
                    }

                    extGot += n;
                }

                extendedHigh = (ext[0] << 8) | ext[1];
            }

            var frame = BlazeFrame.ParseHeader(header, extendedHigh);
            session.Touch();
            // Диагностика 13.10: каждый кадр, включая то, что не дойдёт до Dispatch
            // (сбой разбора TDF, большие кадры).
            session.Log.Info($"[rx] сессия={session.Id} comp=0x{frame.ComponentId:X} " +
                             $"cmd=0x{frame.CommandId:X} len={frame.PayloadLength} id={frame.MessageId}");

            // 2) payload
            var payload = Array.Empty<byte>();
            if (frame.PayloadLength > 0)
            {
                payload = new byte[frame.PayloadLength];
                var got = 0;
                while (got < payload.Length)
                {
                    var n = await stream.ReadAsync(
                        payload.AsMemory(got, payload.Length - got), ct);
                    if (n == 0)
                    {
                        return;
                    }

                    got += n;
                }
            }

            session.Log.Debug($"сессия {session.Id}: кадр " +
                $"{Convert.ToHexString(header)}{Convert.ToHexString(payload)}");

            // 3) обработка может ответить несколько раз
            BlazeFrame.Handle(session, frame, payload);
        }
    }
}