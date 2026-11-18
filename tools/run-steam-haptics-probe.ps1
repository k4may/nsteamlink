# Windows PowerShell 5.1+. No Steam installation/configuration changes.
# Captures requests and host logs; delivery/fidelity need the Switch capture.
param([string]$SteamRoot = '', [int]$AppId = 1049320,
      [string]$SwitchAddress = '', [switch]$CompareHostRpc, [switch]$IsolateGates)
$ErrorActionPreference = 'Stop'
if ($AppId -le 0) { throw 'AppId must be positive.' }
if ($CompareHostRpc -and -not $SwitchAddress) { throw 'SwitchAddress is required for the RPC comparison.' }
if ($IsolateGates -and -not $CompareHostRpc) { throw 'IsolateGates requires CompareHostRpc.' }
$executable = Join-Path $PSScriptRoot 'steam-haptics-probe.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Probe executable missing.' }
if (-not $SteamRoot) {
    $SteamRoot = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam').SteamPath
}
$SteamRoot = (Resolve-Path -LiteralPath $SteamRoot).Path
$dll = Join-Path $SteamRoot 'steamclient64.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw 'Steam DLL missing.' }
$desktop = [Environment]::GetFolderPath('Desktop')
$output = Join-Path $desktop ('nsteamlink-haptics-probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip')
if (Test-Path -LiteralPath $output) { throw 'Output already exists.' }
$stage = Join-Path ([IO.Path]::GetTempPath()) ('nsteamlink-host-probe-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    $manifest = [ordered]@{
        schema = 1
        started_at = (Get-Date).ToString('o')
        app_id = $AppId
        switch_address = $SwitchAddress
        compare_host_rpc = [bool]$CompareHostRpc
        isolate_gates = [bool]$IsolateGates
        steam_dll_sha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
        steam_dll_version = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
        probe_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
        delivery_verified = $false
        physical_fidelity_verified = $false
    }
    $nativePreference = $ErrorActionPreference
    try {
        # Windows PowerShell turns native stderr into error records. Preserve
        # them in the archive even if the probe/Steam DLL reports an error.
        $ErrorActionPreference = 'Continue'
        $probeArguments = @('--steam-root', $SteamRoot, '--appid', "$AppId", '--probe')
        if ($SwitchAddress) { $probeArguments += @('--switch-address', $SwitchAddress) }
        if ($CompareHostRpc) { $probeArguments += '--compare-host-rpc' }
        if ($IsolateGates) { $probeArguments += '--isolate-gates' }
        & $executable @probeArguments 2>&1 |
            Out-File -LiteralPath (Join-Path $stage 'requests.jsonl') -Encoding utf8
        $manifest['exit_code'] = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $nativePreference
    }
    $manifest['completed_at'] = (Get-Date).ToString('o')
    foreach ($name in @('controller.txt', 'streaming_log.txt')) {
        $source = Join-Path $SteamRoot ('logs\' + $name)
        if (Test-Path -LiteralPath $source) {
            Get-Content -LiteralPath $source -Encoding utf8 -Tail 1500 |
                Set-Content -LiteralPath (Join-Path $stage $name) -Encoding utf8
        }
    }
    # Only matched haptics lines, not full account configuration files. These
    # disk values are context; they are not proof of the active runtime state.
    $candidates = @((Join-Path $SteamRoot 'config\config.vdf'))
    $userRoot = Join-Path $SteamRoot 'userdata'
    if (Test-Path -LiteralPath $userRoot) {
        foreach ($account in (Get-ChildItem -LiteralPath $userRoot -Directory -ErrorAction SilentlyContinue)) {
            $candidates += Join-Path $account.FullName 'config\localconfig.vdf'
        }
    }
    $settings = foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            Select-String -LiteralPath $candidate -Pattern 'haptic|rumble|vibration' -ErrorAction SilentlyContinue |
                ForEach-Object { [ordered]@{file = $candidate; line = $_.LineNumber; text = $_.Line} }
        }
    }
    @($settings) | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $stage 'haptics-settings.json') -Encoding utf8
    # If no supported x64 module was found, preserve the companion client DLL
    # for offline layout/architecture research. Never guess its runtime layout.
    $requestsPath = Join-Path $stage 'requests.jsonl'
    $legacyDll = Join-Path $SteamRoot 'steamclient.dll'
    if ((Select-String -LiteralPath $requestsPath -SimpleMatch 'host_state_supported_64bit_module_not_found' -Quiet) -and
        (Test-Path -LiteralPath $legacyDll -PathType Leaf)) {
        try {
            Copy-Item -LiteralPath $legacyDll -Destination (Join-Path $stage 'steamclient-fallback-for-offline.dll')
            $manifest['optional_legacy_dll_sha256'] = (Get-FileHash -LiteralPath $legacyDll -Algorithm SHA256).Hash.ToLowerInvariant()
        } catch {
            $manifest['optional_legacy_collection_error'] = $_.Exception.Message
        }
    }
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $stage 'manifest.json') -Encoding utf8
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $output
    Write-Output "Created $output"
    Write-Output "Probe exit code: $($manifest.exit_code). Delivery still needs Switch trace verification."
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
