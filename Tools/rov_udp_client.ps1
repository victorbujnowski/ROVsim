# Cliente de teste do ROVsim: envia comandos de pilotagem por UDP, 20 vezes por segundo.
# Exemplo:  .\rov_udp_client.ps1 -Surge 1 -Seconds 5
param(
    [string]$HostAddress = '127.0.0.1',
    [int]$Port = 5000,
    [double]$Surge = 0,
    [double]$Sway = 0,
    [double]$Heave = 0,
    [double]$Yaw = 0,
    [double]$Seconds = 5,
    [int]$RateHz = 20
)

# JSON exige ponto decimal (0.5). No Windows em português o padrão seria vírgula (0,5),
# o que geraria um JSON inválido. A cultura "invariante" força o ponto.
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture

$Udp = New-Object System.Net.Sockets.UdpClient
$TotalPackets = [int]($Seconds * $RateHz)
$DelayMs = [int](1000 / $RateHz)

try {
    for ($Seq = 1; $Seq -le $TotalPackets; $Seq++) {
        $Json = [string]::Format($Invariant,
            '{{"type":"cmd","seq":{0},"surge":{1},"sway":{2},"heave":{3},"yaw":{4}}}',
            $Seq, $Surge, $Sway, $Heave, $Yaw)

        $Bytes = [System.Text.Encoding]::UTF8.GetBytes($Json)
        [void]$Udp.Send($Bytes, $Bytes.Length, $HostAddress, $Port)

        Write-Progress -Activity "Enviando para ${HostAddress}:$Port" -Status $Json -PercentComplete (100 * $Seq / $TotalPackets)
        Start-Sleep -Milliseconds $DelayMs
    }
    Write-Host "Enviados $TotalPackets comandos. O ROV vai entrar em failsafe em 0,5 s."
}
finally {
    $Udp.Close()   # sempre libera o socket, mesmo se der erro ou Ctrl+C
}