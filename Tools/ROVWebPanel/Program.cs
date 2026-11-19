// =============================================================================
// Painel web do ROVsim: a "ponte" entre o simulador e o navegador.
//
//   Simulador -> ponte:   UDP 5001        telemetria JSON (10 Hz)
//                         UDP 10001-10004 sensores NMEA (girobússola, atitude, profundímetro, altímetro)
//                         TCP 10010       sensor NMEA do DVL
//   Ponte -> simulador:   TCP 5002        comandos com resposta (MODE, AUTO, CURRENT...)
//                         UDP 5000        pilotagem (surge, sway, heave, yaw)
//   Navegador <-> ponte:  HTTP            a página (wwwroot)
//                         WebSocket /ws   estado a 10 Hz para o navegador; comandos e pilotagem de volta
//
// A ponte fala com o simulador SÓ pelas interfaces de rede dele, como um console de superfície real.
// Rodar:  dotnet run --project Tools/ROVWebPanel   (opcional: --sim 192.168.0.10 --port 8080)
// =============================================================================

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

var simHost = ArgValue(args, "--sim") ?? "127.0.0.1";
var httpPort = int.TryParse(ArgValue(args, "--port"), out var p) ? p : 8080;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.UseUrls($"http://localhost:{httpPort}");
builder.Logging.SetMinimumLevel(LogLevel.Warning);
var app = builder.Build();

var hub = new PanelHub();
var telemetry = new TelemetryLink(5001);
var sensors = new SensorLinks(simHost, new[] { 10001, 10002, 10003, 10004 }, 10010);
var commands = new CommandLink(simHost, 5002, hub);
var pilot = new PilotLink(simHost, 5000);

var stop = app.Lifetime.ApplicationStopping;
_ = telemetry.RunAsync(stop);
_ = sensors.RunAsync(stop);
_ = commands.RunAsync(stop);

// Estado para todos os navegadores, 10 vezes por segundo
_ = Task.Run(async () =>
{
    while (!stop.IsCancellationRequested)
    {
        try
        {
            var state = new JsonObject
            {
                ["type"] = "state",
                ["telemetry"] = telemetry.Latest?.DeepClone(),
                ["telemetryAge"] = telemetry.AgeSeconds,
                ["links"] = new JsonObject
                {
                    ["telemetry"] = telemetry.AgeSeconds < 1.0,
                    ["commands"] = commands.Connected,
                    ["dvl"] = sensors.DvlConnected,
                    ["pilot"] = pilot.Active,
                },
                ["sensors"] = sensors.Snapshot(),
            };
            await hub.BroadcastAsync(state.ToJsonString());
        }
        catch (Exception ex)
        {
            // Um erro num ciclo não pode parar o painel inteiro: registra e segue no próximo
            Console.WriteLine($"Erro ao montar o estado: {ex.Message}");
        }
        await Task.Delay(100, stop).ContinueWith(_ => { });
    }
});

app.UseDefaultFiles();
app.UseStaticFiles();
app.UseWebSockets();

app.Map("/ws", async context =>
{
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = 400;
        return;
    }
    using var socket = await context.WebSockets.AcceptWebSocketAsync();
    var client = hub.Add(socket);
    await hub.SendAsync(client, new JsonObject { ["type"] = "console", ["dir"] = "info", ["text"] = $"Painel conectado à ponte. Simulador em {simHost}." }.ToJsonString());

    // Mensagens do navegador: {"type":"cmd","text":"MODE DEMO"}  ou  {"type":"pilot","surge":..,"sway":..,"heave":..,"yaw":..}
    var buffer = new byte[8192];
    try
    {
        while (socket.State == WebSocketState.Open)
        {
            var result = await socket.ReceiveAsync(buffer, stop);
            if (result.MessageType == WebSocketMessageType.Close) break;

            var message = JsonNode.Parse(Encoding.UTF8.GetString(buffer, 0, result.Count));
            switch ((string?)message?["type"])
            {
                case "cmd":
                    await commands.SendAsync((string?)message!["text"] ?? "");
                    break;
                case "pilot":
                    pilot.Send(Num(message!, "surge"), Num(message!, "sway"), Num(message!, "heave"), Num(message!, "yaw"));
                    break;
            }
        }
    }
    catch (Exception ex) when (ex is WebSocketException or OperationCanceledException or JsonException) { }
    finally
    {
        hub.Remove(client);
    }
});

Console.WriteLine($"Painel do ROVsim: http://localhost:{httpPort}   (simulador em {simHost}; Ctrl+C para sair)");
app.Run();

static string? ArgValue(string[] args, string name)
{
    var index = Array.IndexOf(args, name);
    return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
}

static double Num(JsonNode node, string key) => node[key]?.GetValue<double>() ?? 0.0;

// =============================================================================
// Navegadores conectados
// =============================================================================

sealed class PanelClient(WebSocket socket)
{
    public WebSocket Socket { get; } = socket;
    // Um WebSocket só aceita um envio por vez: o estado (10 Hz) e as respostas do console não podem se cruzar
    public SemaphoreSlim SendLock { get; } = new(1, 1);
}

sealed class PanelHub
{
    private readonly ConcurrentDictionary<PanelClient, byte> _clients = new();

    public PanelClient Add(WebSocket socket)
    {
        var client = new PanelClient(socket);
        _clients[client] = 0;
        return client;
    }

    public void Remove(PanelClient client) => _clients.TryRemove(client, out _);

    public async Task SendAsync(PanelClient client, string json)
    {
        if (client.Socket.State != WebSocketState.Open) return;
        await client.SendLock.WaitAsync();
        try
        {
            await client.Socket.SendAsync(Encoding.UTF8.GetBytes(json), WebSocketMessageType.Text, true, CancellationToken.None);
        }
        catch (WebSocketException) { }
        finally
        {
            client.SendLock.Release();
        }
    }

    public Task BroadcastAsync(string json) => Task.WhenAll(_clients.Keys.Select(client => SendAsync(client, json)));

    public Task ConsoleAsync(string direction, string text) =>
        BroadcastAsync(new JsonObject { ["type"] = "console", ["dir"] = direction, ["text"] = text }.ToJsonString());
}

// =============================================================================
// Simulador -> ponte: telemetria JSON (UDP 5001)
// =============================================================================

sealed class TelemetryLink(int port)
{
    private readonly Stopwatch _sinceLast = new();
    public JsonNode? Latest { get; private set; }
    // Sem pacote ainda: 999 s (JSON não aceita Infinity, e um valor "infinito" derrubaria o envio de estado)
    public double AgeSeconds => _sinceLast.IsRunning ? _sinceLast.Elapsed.TotalSeconds : 999.0;

    public async Task RunAsync(CancellationToken stop)
    {
        using var udp = new UdpClient(port);
        while (!stop.IsCancellationRequested)
        {
            try
            {
                var packet = await udp.ReceiveAsync(stop);
                var json = JsonNode.Parse(Encoding.UTF8.GetString(packet.Buffer));
                if ((string?)json?["type"] == "telemetry")
                {
                    Latest = json;
                    _sinceLast.Restart();
                }
            }
            catch (OperationCanceledException) { break; }
            catch (JsonException) { }   // pacote quebrado: ignora
            catch (SocketException) { } // no Windows, o UDP pode avisar "porta inacessível": ignora
        }
    }
}

// =============================================================================
// Simulador -> ponte: sensores NMEA (UDP e TCP)
// =============================================================================

sealed class SensorStat
{
    public required string Source { get; init; }
    public string Type = "";
    public long Total;
    public long Invalid;
    public string Last = "";
    public readonly Queue<double> RecentTimes = new(); // instantes das frases dos últimos 2 s, para medir a taxa
}

sealed class SensorLinks(string simHost, int[] udpPorts, int tcpPort)
{
    private readonly ConcurrentDictionary<string, SensorStat> _stats = new();
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    public bool DvlConnected { get; private set; }

    public Task RunAsync(CancellationToken stop) =>
        Task.WhenAll(udpPorts.Select(port => RunUdpAsync(port, stop)).Append(RunTcpAsync(stop)));

    private async Task RunUdpAsync(int port, CancellationToken stop)
    {
        using var udp = new UdpClient(port);
        while (!stop.IsCancellationRequested)
        {
            try
            {
                var packet = await udp.ReceiveAsync(stop);
                Record($"UDP {port}", Encoding.ASCII.GetString(packet.Buffer)); // um datagrama = uma frase
            }
            catch (OperationCanceledException) { break; }
            catch (SocketException) { }
        }
    }

    // TCP é um fluxo: separa as frases pelo fim de linha (o framing do NMEA). Reconecta se o simulador cair.
    private async Task RunTcpAsync(CancellationToken stop)
    {
        while (!stop.IsCancellationRequested)
        {
            try
            {
                using var tcp = new TcpClient();
                await tcp.ConnectAsync(simHost, tcpPort, stop);
                DvlConnected = true;
                using var reader = new StreamReader(tcp.GetStream(), Encoding.ASCII);
                while (await reader.ReadLineAsync(stop) is { } line)
                {
                    Record($"TCP {tcpPort}", line);
                }
            }
            catch (OperationCanceledException) { break; }
            catch (Exception) { }
            DvlConnected = false;
            await Task.Delay(2000, stop).ContinueWith(_ => { });
        }
    }

    private void Record(string source, string sentence)
    {
        sentence = sentence.Trim();
        if (sentence.Length == 0) return;

        var type = sentence.StartsWith('$') ? sentence[1..].Split(',', '*')[0] : "?";
        var stat = _stats.GetOrAdd($"{source} {type}", _ => new SensorStat { Source = source });
        lock (stat)
        {
            stat.Type = type;
            stat.Total++;
            if (!Nmea.IsValid(sentence)) stat.Invalid++;
            stat.Last = sentence;
            var now = _clock.Elapsed.TotalSeconds;
            stat.RecentTimes.Enqueue(now);
            while (stat.RecentTimes.Count > 0 && now - stat.RecentTimes.Peek() > 2.0) stat.RecentTimes.Dequeue();
        }
    }

    public JsonArray Snapshot()
    {
        var now = _clock.Elapsed.TotalSeconds;
        var list = new JsonArray();
        foreach (var stat in _stats.Values.OrderBy(s => s.Source, StringComparer.Ordinal))
        {
            lock (stat)
            {
                while (stat.RecentTimes.Count > 0 && now - stat.RecentTimes.Peek() > 2.0) stat.RecentTimes.Dequeue();
                list.Add(new JsonObject
                {
                    ["source"] = stat.Source,
                    ["type"] = stat.Type,
                    ["name"] = Nmea.SensorName(stat.Type),
                    ["hz"] = stat.RecentTimes.Count / 2.0,
                    ["total"] = stat.Total,
                    ["invalid"] = stat.Invalid,
                    ["last"] = stat.Last,
                    ["fields"] = Nmea.Describe(stat.Last),
                });
            }
        }
        return list;
    }
}

static class Nmea
{
    // Checksum: XOR de todos os caracteres entre '$' e '*'
    public static bool IsValid(string sentence)
    {
        var star = sentence.LastIndexOf('*');
        if (!sentence.StartsWith('$') || star < 0 || star + 3 != sentence.Length) return false;
        byte sum = 0;
        foreach (var c in sentence[1..star]) sum ^= (byte)c;
        return sum.ToString("X2") == sentence[(star + 1)..].ToUpperInvariant();
    }

    public static string SensorName(string type) => type switch
    {
        "HEHDT" => "Girobússola",
        "PRDID" => "Sensor de movimento",
        "VDVBW" => "DVL",
        "PRSMD" => "Profundímetro",
        "SDDPT" => "Altímetro",
        _ => "?",
    };

    // Traduz os campos da frase para o painel (o que cada número significa)
    public static string Describe(string sentence)
    {
        var star = sentence.IndexOf('*');
        if (star < 0) return "";
        var f = sentence[1..star].Split(',');
        string At(int i) => i < f.Length && f[i].Length > 0 ? f[i] : "--";
        return f[0] switch
        {
            "HEHDT" => $"rumo {At(1)}°",
            "PRDID" => $"pitch {At(1)}°  roll {At(2)}°  rumo {At(3)}°",
            "VDVBW" => $"água: {At(1)} / {At(2)} nós   fundo: {At(4)} / {At(5)} nós",
            "PRSMD" => $"profundidade {At(1)} m",
            "SDDPT" => $"altitude {At(1)} m",
            _ => "",
        };
    }
}

// =============================================================================
// Ponte -> simulador: comandos (TCP 5002) e pilotagem (UDP 5000)
// =============================================================================

sealed class CommandLink(string simHost, int port, PanelHub hub)
{
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private StreamWriter? _writer;
    public bool Connected { get; private set; }

    public async Task RunAsync(CancellationToken stop)
    {
        while (!stop.IsCancellationRequested)
        {
            try
            {
                using var tcp = new TcpClient();
                await tcp.ConnectAsync(simHost, port, stop);
                var stream = tcp.GetStream();
                _writer = new StreamWriter(stream, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
                Connected = true;
                await hub.ConsoleAsync("info", $"Conectado aos comandos (TCP {port}).");

                // Cada linha que chega é uma resposta (OK/ERR) ou a mensagem de boas-vindas
                using var reader = new StreamReader(stream, Encoding.UTF8);
                while (await reader.ReadLineAsync(stop) is { } line)
                {
                    await hub.ConsoleAsync("in", line);
                }
            }
            catch (OperationCanceledException) { break; }
            catch (Exception) { }
            if (Connected) await hub.ConsoleAsync("info", "Conexão de comandos perdida. Tentando de novo...");
            Connected = false;
            _writer = null;
            await Task.Delay(2000, stop).ContinueWith(_ => { });
        }
    }

    public async Task SendAsync(string command)
    {
        command = command.Trim();
        if (command.Length == 0) return;
        if (_writer is null)
        {
            await hub.ConsoleAsync("info", $"Sem conexão com o simulador: \"{command}\" não foi enviado.");
            return;
        }
        await hub.ConsoleAsync("out", command);
        await _writeLock.WaitAsync();
        try { await _writer.WriteLineAsync(command); }
        catch (Exception) { }
        finally { _writeLock.Release(); }
    }
}

sealed class PilotLink(string simHost, int port)
{
    private readonly UdpClient _udp = new();
    private readonly Stopwatch _sinceLast = new();
    private long _seq;

    // "Ativo" enquanto o navegador estiver mandando comandos (o simulador tem o próprio watchdog de 0,5 s)
    public bool Active => _sinceLast.IsRunning && _sinceLast.Elapsed.TotalSeconds < 0.5;

    public void Send(double surge, double sway, double heave, double yaw)
    {
        var json = string.Format(CultureInfo.InvariantCulture,
            "{{\"type\":\"cmd\",\"seq\":{0},\"surge\":{1:0.###},\"sway\":{2:0.###},\"heave\":{3:0.###},\"yaw\":{4:0.###}}}",
            Interlocked.Increment(ref _seq), Clamp(surge), Clamp(sway), Clamp(heave), Clamp(yaw));
        var bytes = Encoding.UTF8.GetBytes(json);
        _udp.Send(bytes, bytes.Length, simHost, port);
        _sinceLast.Restart();
    }

    private static double Clamp(double v) => Math.Clamp(v, -1.0, 1.0);
}
