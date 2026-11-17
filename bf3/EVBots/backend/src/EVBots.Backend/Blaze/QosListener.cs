// QosListener.cs — UDP-пробы качества связи (QoS).
//
// BF3 клиент шлёт короткие UDP-пакеты на порт QoS, чтобы измерить задержку.
// Рабочий бэкенд это слушает («[qos] listening»). Отвечаем на echo —
// без этого клиент может решить, что канал плохой.

using System.Net;
using System.Net.Sockets;

namespace EVBots.Backend.Blaze;

internal sealed class QosListener(int port, Log log)
{
    private readonly CancellationTokenSource _stop = new();
    private UdpClient? _udp;
    private Task? _loop;

    public void Start()
    {
        _udp = new UdpClient(new IPEndPoint(IPAddress.Any, port));
        _loop = Task.Run(() => LoopAsync(_stop.Token));
        log.Ok($"QoS: listening on UDP {port}");
    }

    public void Stop()
    {
        try
        {
            _stop.Cancel();
            _udp?.Close();
        }
        catch
        {
            // игнорируем
        }
    }

    private async Task LoopAsync(CancellationToken ct)
    {
        var endpoint = new IPEndPoint(IPAddress.Any, 0);

        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult result;
            try
            {
                result = await _udp!.ReceiveAsync(ct);
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
                if (!ct.IsCancellationRequested)
                {
                    log.Debug($"QoS: {ex.SocketErrorCode}");
                }

                continue;
            }

            // echo обратно отправителю
            try
            {
                await _udp.SendAsync(result.Buffer.AsMemory(), result.RemoteEndPoint,
                    ct);
                log.Debug($"QoS: echo {result.Buffer.Length} байт " +
                          $"← {result.RemoteEndPoint}");
            }
            catch (SocketException)
            {
                // клиент мог уйти — это нормально
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }
}