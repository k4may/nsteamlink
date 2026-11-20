$ErrorActionPreference = 'Stop'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$folder = Join-Path $env:LOCALAPPDATA "NSteamLink\HapticsPanel\desktop-$stamp"
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$started = Get-Date
try {
    $process = Start-Process -FilePath (Join-Path $PSScriptRoot 'steam-haptics-panel.exe') `
        -ArgumentList '--desktop' -WorkingDirectory $PSScriptRoot -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $folder 'stdout.txt') `
        -RedirectStandardError (Join-Path $folder 'stderr.txt')
    @{ exit_code = $process.ExitCode; started = $started.ToString('o'); mode = 'desktop_rpc' } |
        ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $folder 'launch.json')
} catch {
    $_ | Out-String | Set-Content -Encoding UTF8 (Join-Path $folder 'launch-error.txt')
} finally {
    $logs = Join-Path $env:LOCALAPPDATA 'NSteamLink\HapticsPanel'
    Get-ChildItem $logs -Filter 'session-*.jsonl' | Where-Object { $_.LastWriteTime -ge $started } |
        Copy-Item -Destination $folder
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        foreach ($name in @('controller.txt', 'console_log.txt', 'streaming_log.txt')) {
            $path = Join-Path $steam "logs\$name"
            if (Test-Path $path) {
                Get-Content $path -Tail 250 | Set-Content -Encoding UTF8 (Join-Path $folder $name)
            }
        }
    }
    $archive = Join-Path $PSScriptRoot "desktop-results-$stamp.zip"
    Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $archive -Force
    Write-Host "Result archive: $archive"
}
