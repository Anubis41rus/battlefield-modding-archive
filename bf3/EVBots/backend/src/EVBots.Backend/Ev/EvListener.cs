// EvListener.cs — TCP-слушатель EV-канала (порт 7828, как у Zeus), backend\EV-PROTOCOL.md §2.1.

using System.Net;
using System.Net.Sockets;

namespace EVBots.Backend.Ev;

internal sealed class EvListener(int port, EvService service, Log log)
{
    private readonly CancellationTokenSource _stop = new();
    private TcpListener? _tcp;

    public int Port => ((IPEndPoint?)_tcp?.LocalEndpoint)?.Port ?? port;

    public void Start()
    {
        _tcp = new TcpListener(IPAddress.Any, port);
        _tcp.Start();
        _ = Task.Run(() => AcceptLoopAsync(_stop.Token));
        log.Ok($"EV: listening on TCP {Port}");
    }

    public void Stop()
    {
        _stop.Cancel();
        try
        {
            _tcp?.Stop();
        }
        catch
        {
            // игнорируем
        }
    }

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await _tcp!.AcceptTcpClientAsync(ct);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (SocketException ex)
            {
                log.Warn($"EV: accept: {ex.Message}");
                continue;
            }

            _ = Task.Run(() => ServeAsync(client, ct), ct);
        }
    }

    private async Task ServeAsync(TcpClient client, CancellationToken ct)
    {
        var session = new EvSession();
        var peer = client.Client.RemoteEndPoint?.ToString() ?? "?";
        log.Debug($"EV: {peer} подключился (сессия {session.Id})");
        try
        {
            using (client)
            {
                client.NoDelay = true;
                var stream = client.GetStream();
                session.Output = stream;   // уведомления из других сессий пишут сюда же (под замком сессии)
                while (!ct.IsCancellationRequested)
                {
                    var req = await EvFrame.ReadAsync(stream, ct);
                    if (req is null)
                    {
                        break;
                    }

                    log.Debug($"EV <- {peer}: {req}");
                    var resp = await service.HandleAsync(session, req.Value);
                    log.Debug($"EV -> {peer}: {resp}");
                    await session.SendAsync(resp, ct);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // остановка
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or SocketException
                                       or System.Text.Json.JsonException)
        {
            log.Warn($"EV: {peer}: {ex.Message}");
        }

        session.Output = null;
        await service.DisconnectedAsync(session);
        log.Debug($"EV: {peer} отключился (сессия {session.Id}{(session.Account is null ? "" : ", " + session.Account.Login)})");
    }
}
