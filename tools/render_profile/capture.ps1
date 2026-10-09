param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [ValidateSet('normal', 'depth_only', 'static_off')][string]$Mode = 'normal',
    [string]$OutputDirectory = (Join-Path $env:TEMP 'orca-render-profile'),
    [string]$Label = '3M-model',
    [ValidateRange(1, 120)][int]$Stride = 4,
    [switch]$CpuOnly
)
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw 'Exe must be a file.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$log = Join-Path $directory ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $Mode + '.log')
$changes = @{
    ORCA_RENDER_PROFILE = '1'
    ORCA_RENDER_PROFILE_MODE = $Mode
    ORCA_RENDER_PROFILE_OUT = $log
    ORCA_RENDER_PROFILE_LABEL = $Label
    ORCA_RENDER_PROFILE_STRIDE = [string]$Stride
    ORCA_RENDER_PROFILE_CPU_ONLY = $(if ($CpuOnly) { '1' } else { '0' })
}
$saved = @{}
foreach ($name in $changes.Keys) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Write-Host 'Close all existing Orca instances before capturing (single-instance forwarding cannot change their environment).'
Write-Host "Mode: $Mode; stride: $Stride; output: $log"
try {
    foreach ($name in $changes.Keys) {
        [Environment]::SetEnvironmentVariable($name, $changes[$name], 'Process')
    }
    $process = Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath) -Wait -PassThru
    Write-Host "Process exit code: $($process.ExitCode)"
} finally {
    foreach ($name in $changes.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process')
    }
}
if (-not (Test-Path -LiteralPath $log)) {
    throw 'No profile log was created. Check the SLIC3R_RENDER_PROFILE=ON build, selected executable, and existing instances.'
}
Write-Host "Capture saved: $log"
