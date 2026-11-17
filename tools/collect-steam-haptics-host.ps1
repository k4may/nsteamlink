# Read-only collection of current Steam binaries, haptics settings and logs.
# Windows PowerShell 5.1+. Does not stop Steam or edit its installation/config.
param(
    [string]$SteamRoot = '',
    [string]$OutputPath = '',
    [int]$AppId = 1049320
)
$ErrorActionPreference = 'Stop'
if ($AppId -le 0) { throw 'AppId must be positive.' }
if (-not $SteamRoot) {
    $SteamRoot = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam').SteamPath
}
$SteamRoot = (Resolve-Path -LiteralPath $SteamRoot).Path.TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath (Join-Path $SteamRoot 'steamclient64.dll'))) {
    throw "steamclient64.dll not found in $SteamRoot"
}
if (-not $OutputPath) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    $OutputPath = Join-Path $desktop ('steam-haptics-host-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip')
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) { throw "Output already exists: $OutputPath" }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('nsteamlink-haptics-' + [Guid]::NewGuid().ToString('N'))
$files = New-Object 'System.Collections.Generic.List[object]'
$missing = New-Object 'System.Collections.Generic.List[string]'
function Copy-Evidence([string]$Source, [string]$Relative) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        $missing.Add($Relative)
        return
    }
    $destination = Join-Path $stage $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination
    $item = Get-Item -LiteralPath $destination
    $files.Add([ordered]@{
        path = $Relative
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        file_version = $item.VersionInfo.FileVersion
    })
}
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    foreach ($relative in @('steamclient64.dll', 'steamclient.dll', 'SDL3.dll', 'SDL2.dll',
        'controller_base\templates\controller_switch_pro.vdf',
        'logs\controller.txt', 'logs\controller.previous.txt', 'logs\controller_ui.txt',
        'logs\streaming_log.txt', 'logs\remote_connections.txt')) {
        Copy-Evidence (Join-Path $SteamRoot $relative) $relative
    }
    # Current Steam Controller Configs can live in any Steam library.
    $libraries = @($SteamRoot)
    $libraryFile = Join-Path $SteamRoot 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $libraryFile) {
        foreach ($match in [regex]::Matches((Get-Content -LiteralPath $libraryFile -Raw), '"path"\s+"([^"]+)"')) {
            $libraries += $match.Groups[1].Value.Replace('\\', '\')
        }
    }
    $libraryIndex = 0
    foreach ($library in ($libraries | Select-Object -Unique)) {
        $configRoot = Join-Path $library 'steamapps\common\Steam Controller Configs'
        if (Test-Path -LiteralPath $configRoot) {
            foreach ($account in (Get-ChildItem -LiteralPath $configRoot -Directory)) {
                $gameRoot = Join-Path $account.FullName ('config\' + $AppId)
                if (Test-Path -LiteralPath $gameRoot) {
                    foreach ($vdf in (Get-ChildItem -LiteralPath $gameRoot -Filter '*.vdf' -File -Recurse)) {
                        $relative = $vdf.FullName.Substring($gameRoot.Length).TrimStart('\', '/')
                        Copy-Evidence $vdf.FullName (Join-Path "game-config\library-$libraryIndex\$($account.Name)" $relative)
                    }
                }
            }
        }
        $libraryIndex++
    }
    # Older per-game config location; only this game's VDF files are selected.
    $userRoot = Join-Path $SteamRoot 'userdata'
    if (Test-Path -LiteralPath $userRoot) {
        foreach ($account in (Get-ChildItem -LiteralPath $userRoot -Directory)) {
            $localConfig = Join-Path $account.FullName 'config\localconfig.vdf'
            if (Test-Path -LiteralPath $localConfig) {
                $settings = Select-String -LiteralPath $localConfig -Pattern 'haptic|rumble|vibration' |
                    ForEach-Object { $_.Line }
                @($settings) | Set-Content -LiteralPath (Join-Path $stage ("preferences-$($account.Name)-haptics.txt")) -Encoding UTF8
            }
            $gameRoot = Join-Path $account.FullName ("241100\remote\controller_config\$AppId")
            if (Test-Path -LiteralPath $gameRoot) {
                foreach ($vdf in (Get-ChildItem -LiteralPath $gameRoot -Filter '*.vdf' -File -Recurse)) {
                    $relative = $vdf.FullName.Substring($gameRoot.Length).TrimStart('\', '/')
                    Copy-Evidence $vdf.FullName (Join-Path "game-config\legacy\$($account.Name)" $relative)
                }
            }
        }
    }
    $globalConfig = Join-Path $SteamRoot 'config\config.vdf'
    if (Test-Path -LiteralPath $globalConfig) {
        $settings = Select-String -LiteralPath $globalConfig -Pattern 'haptic|rumble|vibration' |
            ForEach-Object { $_.Line }
        @($settings) | Set-Content -LiteralPath (Join-Path $stage 'global-haptics-settings.txt') -Encoding UTF8
    }
    [ordered]@{
        schema = 1
        collected_at = (Get-Date).ToString('o')
        app_id = $AppId
        files = @($files.ToArray())
        missing_optional_files = @($missing.ToArray())
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding UTF8
    New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $OutputPath -CompressionLevel Fastest
    Write-Output "Created $OutputPath"
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
