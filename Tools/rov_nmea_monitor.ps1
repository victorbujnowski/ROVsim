# Monitor NMEA do ROVsim: escuta os sensores emulados e confere o checksum e a taxa de cada frase.
#   UDP 10001 (girobussola $HEHDT) e 10002 (atitude $PRDID)
#   UDP 10003 (profundimetro $PRSMD) e 10004 (altimetro $SDDPT): reserva enquanto a porta COM nao existir
#   TCP 10010 (DVL $VDVBW)
#   COM, se informada (profundimetro e altimetro): o OUTRO lado do par de portas, ex.: COM21
# Exemplos:
#   .\rov_nmea_monitor.ps1
#   .\rov_nmea_monitor.ps1 -ComPort COM21 -Seconds 20
param(
    [int[]]$UdpPorts = @(10001, 10002, 10003, 10004),
    [string]$TcpHost = '127.0.0.1',
    [int]$TcpPort = 10010,
    [string]$ComPort = '',
    [int]$BaudRate = 9600,
    [int]$Seconds = 0          # 0 = ate apertar Ctrl+C
)

# Checksum NMEA: XOR de todos os caracteres entre '$' e '*'
function Test-NmeaChecksum([string]$Line) {
    if ($Line -notmatch '^\$(.+)\*([0-9A-Fa-f]{2})$') { return $false }
    $Body = $Matches[1]
    $Expected = [Convert]::ToInt32($Matches[2], 16)
    $Sum = 0
    foreach ($Char in $Body.ToCharArray()) { $Sum = $Sum -bxor [int]$Char }
    return ($Sum -eq $Expected)
}

$Stats = [ordered]@{}
function Add-Sentence([string]$Source, [string]$Line) {
    $Line = $Line.Trim()
    if (-not $Line) { return }
    $Type = if ($Line -match '^\$([A-Z]+)') { $Matches[1] } else { '?' }
    $Key = "$Source $Type"
    if (-not $Stats.Contains($Key)) {
        $Stats[$Key] = [pscustomobject]@{ Fonte = $Source; Frase = $Type; Total = 0; Invalidas = 0; Ultima = '' }
    }
    $Entry = $Stats[$Key]
    $Entry.Total++
    if (-not (Test-NmeaChecksum $Line)) { $Entry.Invalidas++ }
    $Entry.Ultima = $Line
}

# Separa um fluxo (TCP ou serial) em linhas: o NMEA termina cada frase com CR LF
function Split-Stream([string]$Source, [ref]$Buffer, [string]$NewText) {
    $Buffer.Value += $NewText
    while (($Index = $Buffer.Value.IndexOf("`n")) -ge 0) {
        Add-Sentence $Source $Buffer.Value.Substring(0, $Index)
        $Buffer.Value = $Buffer.Value.Substring($Index + 1)
    }
}

# --- Abre as entradas ---
$UdpClients = @()
foreach ($Port in $UdpPorts) { $UdpClients += [pscustomobject]@{ Port = $Port; Client = (New-Object System.Net.Sockets.UdpClient($Port)) } }

$Tcp = $null; $TcpStream = $null; $TcpBuffer = ''
try { $Tcp = New-Object System.Net.Sockets.TcpClient($TcpHost, $TcpPort); $TcpStream = $Tcp.GetStream() }
catch { Write-Host "TCP ${TcpHost}:$TcpPort indisponivel (o simulador esta em Play?)" -ForegroundColor Yellow }

$Serial = $null; $SerialBuffer = ''
if ($ComPort) {
    try { $Serial = New-Object System.IO.Ports.SerialPort($ComPort, $BaudRate, 'None', 8, 'One'); $Serial.Open() }
    catch { Write-Host "Nao consegui abrir $ComPort : $($_.Exception.Message)" -ForegroundColor Yellow; $Serial = $null }
}

$Encoding = [System.Text.Encoding]::ASCII
$Remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
$Start = Get-Date
$NextDraw = $Start

try {
    while ($true) {
        foreach ($Udp in $UdpClients) {
            while ($Udp.Client.Available -gt 0) {
                $Bytes = $Udp.Client.Receive([ref]$Remote)
                Add-Sentence "UDP $($Udp.Port)" $Encoding.GetString($Bytes)   # um datagrama = uma frase
            }
        }
        if ($TcpStream -and $TcpStream.DataAvailable) {
            $Chunk = New-Object byte[] 4096
            $Count = $TcpStream.Read($Chunk, 0, $Chunk.Length)
            Split-Stream "TCP $TcpPort" ([ref]$TcpBuffer) $Encoding.GetString($Chunk, 0, $Count)
        }
        if ($Serial -and $Serial.BytesToRead -gt 0) {
            Split-Stream $ComPort ([ref]$SerialBuffer) $Serial.ReadExisting()
        }

        $Now = Get-Date
        $Elapsed = ($Now - $Start).TotalSeconds
        if ($Now -ge $NextDraw) {
            $NextDraw = $Now.AddSeconds(1)
            Clear-Host
            Write-Host ("Monitor NMEA do ROVsim - {0:N0} s   (Ctrl+C para sair)" -f $Elapsed) -ForegroundColor Cyan
            $Stats.Values | Select-Object Fonte, Frase, Total,
                @{ Name = 'Hz'; Expression = { [Math]::Round($_.Total / [Math]::Max($Elapsed, 0.001), 1) } },
                Invalidas, Ultima | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
        }
        if ($Seconds -gt 0 -and $Elapsed -ge $Seconds) { break }
        Start-Sleep -Milliseconds 10
    }
}
finally {
    foreach ($Udp in $UdpClients) { $Udp.Client.Close() }
    if ($Tcp) { $Tcp.Close() }
    if ($Serial) { $Serial.Close() }
}
