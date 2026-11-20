param([switch]$ExportOnly, [string]$SessionPath = '', [switch]$OpenFolder)
$ErrorActionPreference = 'Stop'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$root = Join-Path $env:LOCALAPPDATA 'NSteamLink\HapticsPanel'
$folder = Join-Path $root "desktop-$stamp"
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$started = Get-Date
$issues = New-Object System.Collections.Generic.List[string]
try {
    if (-not $ExportOnly) {
        $process = Start-Process -FilePath (Join-Path $PSScriptRoot 'steam-haptics-panel.exe') `
            -ArgumentList '--desktop' -WorkingDirectory $PSScriptRoot -PassThru -Wait `
            -RedirectStandardOutput (Join-Path $folder 'stdout.txt') `
            -RedirectStandardError (Join-Path $folder 'stderr.txt')
        @{ exit_code = $process.ExitCode; started = $started.ToString('o'); mode = 'desktop_rpc' } |
            ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $folder 'launch.json')
    }
} catch {
    $issues.Add(($_ | Out-String))
} finally {
    try {
        if ($SessionPath) {
            Copy-Item -LiteralPath $SessionPath -Destination $folder
        } else {
            Get-ChildItem $root -Filter 'session-*.jsonl' | Where-Object { $_.LastWriteTime -ge $started } |
                Copy-Item -Destination $folder
        }
    } catch { $issues.Add(($_ | Out-String)) }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        foreach ($name in @('controller.txt', 'console_log.txt', 'streaming_log.txt')) {
            try {
                $path = Join-Path $steam "logs\$name"
                if (Test-Path $path) {
                    Get-Content $path -Tail 500 | Set-Content -Encoding UTF8 (Join-Path $folder $name)
                }
            } catch { $issues.Add(($_ | Out-String)) }
        }
        foreach ($name in @('streaming_steamdesktop.vdf', 'basicui_gamepad.vdf')) {
            try {
                $path = Join-Path $steam "controller_base\$name"
                if (Test-Path $path) { Copy-Item -LiteralPath $path -Destination $folder }
            } catch { $issues.Add(($_ | Out-String)) }
        }
    }
    @{ export_only = [bool]$ExportOnly; source_session = $SessionPath; errors = @($issues.ToArray()) } |
        ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $folder 'export.json')
    $exports = Join-Path $root 'Exports'
    New-Item -ItemType Directory -Path $exports -Force | Out-Null
    $archive = Join-Path $exports "desktop-results-$stamp.zip"
    Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $archive -Force
    try {
        $beside = Join-Path $PSScriptRoot "desktop-results-$stamp.zip"
        Copy-Item -LiteralPath $archive -Destination $beside -Force
        $archive = $beside
    } catch { Write-Host "Package folder is not writable; exported to $archive" }
    Write-Host "Result archive: $archive"
    if ($OpenFolder -or -not $ExportOnly) {
        Start-Process explorer.exe -ArgumentList ('/select,"' + $archive + '"')
    }
}
