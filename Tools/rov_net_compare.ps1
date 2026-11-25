# Compara a trilha do ROV no HOST e no OBSERVADOR (linhas "NETTRACE" dos dois logs) e mede:
#   - o ATRASO: o deslocamento de tempo que melhor alinha as duas trilhas
#   - o ERRO que sobra depois de alinhar (o observador mostra o ROV no lugar certo, só atrasado?)
#
# Os dois jogos precisam ter rodado com -ROVNetTrace, na MESMA máquina (o relógio do log é o mesmo).
#   .\rov_net_compare.ps1 -HostLog ..\Saved\Logs\NetHost.log -ObserverLog ..\Saved\Logs\NetClient.log
param(
    [Parameter(Mandatory)] [string]$HostLog,
    [Parameter(Mandatory)] [string]$ObserverLog,
    [int]$MaxShiftMs = 1500,       # maior atraso testado
    [double]$MaxRmsErrorM = 0.25,  # critério: erro RMS depois de alinhar
    [double]$SkipFirstSeconds = 3,  # ignora o começo do observador (a cópia do ROV ainda convergindo)
    [string]$From = '',           # opcional: só compara a partir desta hora do log (HH:mm:ss)
    [string]$To = ''              # opcional: só compara até esta hora do log
)

$Invariant = [System.Globalization.CultureInfo]::InvariantCulture
$Pattern = '^\[\d{4}\.\d\d\.\d\d-(\d\d)\.(\d\d)\.(\d\d):(\d{3})\].*NETTRACE (HOST|OBS) pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\)'

# Lê as amostras (tempo em segundos do dia, posição em m)
function Read-Trace([string]$Path, [string]$Who) {
    $Samples = New-Object System.Collections.Generic.List[object]
    # Abre em modo compartilhado: o jogo pode ainda estar escrevendo no log
    $Stream = New-Object System.IO.FileStream((Resolve-Path $Path), [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $Reader = New-Object System.IO.StreamReader($Stream)
    $Lines = $Reader.ReadToEnd() -split "`r?`n"
    $Reader.Close()
    foreach ($Line in $Lines) {
        $M = [regex]::Match($Line, $Pattern)
        if (-not $M.Success -or $M.Groups[5].Value -ne $Who) { continue }
        $T = [int]$M.Groups[1].Value * 3600 + [int]$M.Groups[2].Value * 60 + [int]$M.Groups[3].Value + [int]$M.Groups[4].Value / 1000.0
        $Samples.Add([pscustomobject]@{
            T = $T
            X = [double]::Parse($M.Groups[6].Value, $Invariant)
            Y = [double]::Parse($M.Groups[7].Value, $Invariant)
            Z = [double]::Parse($M.Groups[8].Value, $Invariant)
        })
    }
    return ,$Samples.ToArray()
}

$HostTrace = Read-Trace $HostLog 'HOST'
$ObsTrace = Read-Trace $ObserverLog 'OBS'
if ($ObsTrace.Count -gt 0) { $Start = $ObsTrace[0].T; $ObsTrace = @($ObsTrace | Where-Object { $_.T -ge $Start + $SkipFirstSeconds }) }
if ($From) { $F0 = [TimeSpan]::Parse($From).TotalSeconds; $ObsTrace = @($ObsTrace | Where-Object { $_.T -ge $F0 }) }
if ($To) { $T0 = [TimeSpan]::Parse($To).TotalSeconds; $ObsTrace = @($ObsTrace | Where-Object { $_.T -le $T0 }) }
if ($HostTrace.Count -lt 10 -or $ObsTrace.Count -lt 10) {
    Write-Host "Amostras insuficientes (host $($HostTrace.Count), observador $($ObsTrace.Count)). Rodou com -ROVNetTrace?"
    exit 1
}

# Posição do host num instante qualquer: interpolação linear entre as amostras vizinhas (10 Hz)
function Get-HostPosition([double]$T) {
    $Lo = 0; $Hi = $HostTrace.Count - 1
    if ($T -lt $HostTrace[$Lo].T -or $T -gt $HostTrace[$Hi].T) { return $null }
    while ($Hi - $Lo -gt 1) {                       # busca binária
        $Mid = [int](($Lo + $Hi) / 2)
        if ($HostTrace[$Mid].T -le $T) { $Lo = $Mid } else { $Hi = $Mid }
    }
    $A = $HostTrace[$Lo]; $B = $HostTrace[$Hi]
    $F = if ($B.T -gt $A.T) { ($T - $A.T) / ($B.T - $A.T) } else { 0 }
    return @(($A.X + ($B.X - $A.X) * $F), ($A.Y + ($B.Y - $A.Y) * $F), ($A.Z + ($B.Z - $A.Z) * $F))
}

# Erro RMS e máximo supondo que o observador mostra o host de "Shift" segundos atrás
function Measure-Error([double]$Shift) {
    $Sum = 0.0; $Max = 0.0; $N = 0
    foreach ($S in $ObsTrace) {
        $P = Get-HostPosition ($S.T - $Shift)
        if ($null -eq $P) { continue }
        $D = [math]::Sqrt([math]::Pow($S.X - $P[0], 2) + [math]::Pow($S.Y - $P[1], 2) + [math]::Pow($S.Z - $P[2], 2))
        $Sum += $D * $D; $N++
        if ($D -gt $Max) { $Max = $D }
    }
    if ($N -eq 0) { return $null }
    return [pscustomobject]@{ Shift = $Shift; Rms = [math]::Sqrt($Sum / $N); Max = $Max; N = $N }
}

$Results = for ($Ms = 0; $Ms -le $MaxShiftMs; $Ms += 10) { Measure-Error ($Ms / 1000.0) }
$Results = $Results | Where-Object { $_ }
$Best = $Results | Sort-Object Rms | Select-Object -First 1
$Zero = $Results | Where-Object { $_.Shift -eq 0 } | Select-Object -First 1

Write-Host ("Amostras: host {0}, observador {1} (comparadas {2})" -f $HostTrace.Count, $ObsTrace.Count, $Best.N)
Write-Host ("Sem alinhar (atraso 0):   erro RMS {0:N3} m, máximo {1:N3} m" -f $Zero.Rms, $Zero.Max)
Write-Host ("Atraso medido:            {0:N0} ms" -f ($Best.Shift * 1000))
Write-Host ("Depois de alinhar:        erro RMS {0:N3} m, máximo {1:N3} m" -f $Best.Rms, $Best.Max)

# Os piores instantes (horário do log): ajuda a achar o que aconteceu ali no roteiro do host
$Worst = foreach ($S in $ObsTrace) {
    $P = Get-HostPosition ($S.T - $Best.Shift)
    if ($null -eq $P) { continue }
    [pscustomobject]@{
        Hora = [TimeSpan]::FromSeconds($S.T).ToString('hh\:mm\:ss\.fff')
        Erro = [math]::Sqrt([math]::Pow($S.X - $P[0], 2) + [math]::Pow($S.Y - $P[1], 2) + [math]::Pow($S.Z - $P[2], 2))
    }
}
Write-Host "Piores instantes (hora do log, erro em m):"
$Worst | Sort-Object Erro -Descending | Select-Object -First 5 | ForEach-Object { Write-Host ("  {0}  {1:N3}" -f $_.Hora, $_.Erro) }

$Passed = $Best.Rms -lt $MaxRmsErrorM
Write-Host ("NET CHECK observador fiel ao host: {0} (limite RMS {1} m)" -f $(if ($Passed) { 'PASSOU' } else { 'FALHOU' }), $MaxRmsErrorM)
if (-not $Passed) { exit 1 }
