# Monitor de telemetria do ROVsim: escuta a porta 5001 e mostra os dados recebidos.
# Pare com Ctrl+C.
param([int]$Port = 5001)

$Invariant = [System.Globalization.CultureInfo]::InvariantCulture

$Udp = New-Object System.Net.Sockets.UdpClient($Port)  # "bind": este programa passa a receber o que chega na 5001
$Udp.Client.ReceiveTimeout = 1000                        # Receive() desiste após 1 s, para o Ctrl+C funcionar
$Remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)

$Received = 0
$Lost = 0
$LastSeq = 0

Write-Host "Escutando telemetria na porta $Port (Ctrl+C para sair)..."
try {
    while ($true) {
        try {
            $Bytes = $Udp.Receive([ref]$Remote)
        }
        catch [System.Net.Sockets.SocketException] {
            Write-Host "`rAguardando o simulador...                                              " -NoNewline
            continue
        }

        $T = [System.Text.Encoding]::UTF8.GetString($Bytes) | ConvertFrom-Json
        if ($T.type -ne 'telemetry') { continue }

        # Conta pacotes perdidos pelos "buracos" na sequência
        $Received++
        if ($LastSeq -gt 0 -and $T.seq -gt $LastSeq + 1) { $Lost += $T.seq - $LastSeq - 1 }
        $LastSeq = $T.seq

        $Speed = [Math]::Sqrt($T.vel[0] * $T.vel[0] + $T.vel[1] * $T.vel[1])
        $RemoteTag = if ($T.remote) { ' REMOTO' } else { '' }

        $Line = [string]::Format($Invariant,
            "`r[{0}{1}] prof {2,6:F1} m | alt {3,5:F1} m | rumo {4,5:F1} | vel {5,4:F2} m/s ({6,4:F2} nos) | heave {7,5:F2} | seq {8} | perdidos {9}   ",
            $T.mode, $RemoteTag, $T.depth, $T.altitude, $T.heading, $Speed, ($Speed / 0.514444), $T.vel[2], $T.seq, $Lost)
        Write-Host $Line -NoNewline
    }
}
finally {
    $Udp.Close()
    Write-Host "`nRecebidos: $Received   Perdidos: $Lost"
}