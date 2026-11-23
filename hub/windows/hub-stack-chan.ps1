# hub-stack-chan — mantém o Hub do AI Monitor (e, opcionalmente, o canal de voz local) no ar.
# Iniciado no login do Windows por um .vbs na pasta Inicializar (janela oculta).
# Uma instância só; verifica a cada 15 s e religa o que tiver caído.
param(
    [string]$CanalDir = "",          # pasta do canal de voz (com voz.cmd); vazio = não gerencia
    [int]$HubPort = 8790,
    [int]$CanalPort = 4850,
    [string]$LogDir = ""             # vazio = %LOCALAPPDATA%\hub-stack-chan
)
$ErrorActionPreference = "Continue"
$hubDir = Split-Path -Parent $PSScriptRoot
$logDir = if ($LogDir) { $LogDir } else { Join-Path $env:LOCALAPPDATA "hub-stack-chan" }
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "supervisor.log"
function Log($m) { "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $m | Add-Content -Path $log -Encoding utf8 }

$mutex = New-Object System.Threading.Mutex($false, "Local\hub-stack-chan")
if (-not $mutex.WaitOne(0)) { exit 0 }

function Listening($port) { [bool](Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue) }

function Start-Hub {
    $py = Join-Path $hubDir ".venv\Scripts\python.exe"
    $cfg = Join-Path $hubDir "config.yaml"
    if (-not (Test-Path $py) -or -not (Test-Path $cfg)) { Log "hub: falta .venv ou config.yaml em $hubDir"; return }
    $env:PYTHONPATH = $null
    $out = Join-Path $logDir "hub.log"
    if ((Test-Path $out) -and (Get-Item $out).Length -gt 20MB) { Move-Item -Force $out "$out.1" }
    Start-Process -FilePath $py -ArgumentList "-m", "hub", "--config", "`"$cfg`"" -WorkingDirectory $hubDir `
        -WindowStyle Hidden -RedirectStandardOutput $out -RedirectStandardError "$out.err" | Out-Null
    Log "hub: iniciado (porta $HubPort)"
}

function Start-Canal {
    $cmd = Join-Path $CanalDir "voz.cmd"
    if (-not (Test-Path $cmd)) { Log "canal: $cmd não existe"; return }
    $out = Join-Path $logDir "canal.log"
    $a = '/s /c ""' + $cmd + '" >> "' + $out + '" 2>&1"'
    Start-Process -FilePath $env:ComSpec -ArgumentList $a -WorkingDirectory $CanalDir -WindowStyle Hidden | Out-Null
    Log "canal: iniciado (porta $CanalPort)"
}

Log "supervisor no ar (hub $hubDir, canal '$CanalDir')"
$hubStarted = [datetime]::MinValue; $canalStarted = [datetime]::MinValue
while ($true) {
    $now = Get-Date
    if ($CanalDir -and -not (Listening $CanalPort) -and ($now - $canalStarted).TotalSeconds -gt 90) {
        Start-Canal; $canalStarted = $now
    }
    if (-not (Listening $HubPort) -and ($now - $hubStarted).TotalSeconds -gt 60) {
        Start-Hub; $hubStarted = $now
    }
    Start-Sleep -Seconds 15
}
