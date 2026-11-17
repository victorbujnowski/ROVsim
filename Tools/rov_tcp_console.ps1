# Console TCP do ROVsim: conecta na porta 5002 e envia os comandos digitados.
# Digite SAIR para fechar.
param(
    [string]$HostAddress = '127.0.0.1',
    [int]$Port = 5002
)

$Client = New-Object System.Net.Sockets.TcpClient
try {
    $Client.Connect($HostAddress, $Port)   # TCP: "liga" para o simulador e falha na hora se ninguem atender
}
catch {
    Write-Host "Nao conectou em ${HostAddress}:$Port. O simulador esta em Play?" -ForegroundColor Red
    exit 1
}

$Stream = $Client.GetStream()
$Reader = New-Object System.IO.StreamReader($Stream, [System.Text.Encoding]::UTF8)
# UTF8Encoding($false) = UTF-8 SEM BOM (o BOM seriam 3 bytes invisiveis grudados no primeiro comando)
$Writer = New-Object System.IO.StreamWriter($Stream, (New-Object System.Text.UTF8Encoding($false)))
$Writer.NewLine = "`n"     # o servidor separa os comandos por '\n'
$Writer.AutoFlush = $true   # envia na hora, sem esperar juntar mais texto

try {
    Write-Host $Reader.ReadLine() -ForegroundColor Cyan   # mensagem de boas-vindas
    while ($true) {
        $Command = Read-Host 'ROV'
        if ($Command -eq 'SAIR') { break }
        if ([string]::IsNullOrWhiteSpace($Command)) { continue }

        $Writer.WriteLine($Command)
        $Reply = $Reader.ReadLine()          # espera a resposta: TCP garante que ela vem
        if ($null -eq $Reply) {
            Write-Host 'Conexao fechada pelo simulador.' -ForegroundColor Red
            break
        }
        $Color = if ($Reply.StartsWith('OK')) { 'Green' } else { 'Yellow' }
        Write-Host $Reply -ForegroundColor $Color
    }
}
finally {
    $Client.Close()   # "desliga o telefone": o servidor ve o Recv retornar 0 bytes
}