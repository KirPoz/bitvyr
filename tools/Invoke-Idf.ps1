<#
.SYNOPSIS
Запускает ESP-IDF в активированном окружении для единственного каталога build.
.DESCRIPTION
Использует выбранные IDF_PATH/IDF_PYTHON_ENV_PATH без изменения глобального SDK.
Build, Full Clean и аппаратная прошивка остаются независимыми действиями.
#>
param(
    [Parameter(Mandatory)]
    [string]$ProjectDirectory,
    [ValidateSet('build','fullclean','reconfigure','version')]
    [string]$Action = 'build',
    [ValidateSet('ON','OFF')]
    [string]$Overclock
)
$ErrorActionPreference = 'Stop'
$taskProjectRoot = (Resolve-Path -LiteralPath $ProjectDirectory).ProviderPath
if (-not $env:IDF_PATH) { throw 'Open an activated ESP-IDF 6.1 terminal first.' }
$taskIdf = Join-Path $env:IDF_PATH 'tools/idf.py'
if (-not (Test-Path -LiteralPath $taskIdf -PathType Leaf)) { throw 'IDF_PATH does not contain tools/idf.py.' }
$taskPython = 'python'
if ($env:IDF_PYTHON_ENV_PATH) {
    $taskPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts/python.exe'
    if (-not (Test-Path -LiteralPath $taskPython -PathType Leaf)) {
        $taskPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'bin/python'
    }
}
# Дочерняя конфигурация загрузчика ESP-Hosted читает версию SDK из окружения.
$env:ESP_IDF_VERSION = '6.1'
$taskArguments = @('-B','build')
if ($Overclock) { $taskArguments += @('-D',"NES_P4_CPU_OVERCLOCK=$Overclock") }
if ($Action -eq 'version') { $taskArguments = @('--version') } else { $taskArguments += $Action }
Push-Location $taskProjectRoot
try {
    # PowerShell5 не должен превращать обычные заметки Kconfig в исключения.
    $taskPreviousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $taskPython $taskIdf @taskArguments; $taskResult = $LASTEXITCODE }
    finally { $ErrorActionPreference = $taskPreviousPreference }
} finally { Pop-Location }
exit $taskResult
