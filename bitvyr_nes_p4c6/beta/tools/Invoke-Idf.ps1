<# Указывает редакцию для общего запуска ESP-IDF из корня Bitvyr. #>
param(
    [ValidateSet('build','fullclean','reconfigure','version')]
    [string]$Action = 'build',
    [ValidateSet('ON','OFF')]
    [string]$Overclock
)
$taskLauncher = Join-Path $PSScriptRoot '../../../tools/Invoke-Idf.ps1'
& $taskLauncher -ProjectDirectory (Split-Path -Parent $PSScriptRoot) @PSBoundParameters
exit $LASTEXITCODE
