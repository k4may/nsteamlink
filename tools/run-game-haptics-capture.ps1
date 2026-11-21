param([ValidateRange(1,3600)][int]$Seconds = 60, [string]$Address = '10.10.10.111')
$ErrorActionPreference = 'Stop'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$root = Join-Path $env:LOCALAPPDATA 'NSteamLink\GameHaptics'
$folder = Join-Path $root "game-$stamp"
New-Item -ItemType Directory -Path $folder -Force | Out-Null
$started = Get-Date
$issues = New-Object System.Collections.Generic.List[string]
$job = $null
$process = $null
$endpointJob = $null
$endpointFile = Join-Path $folder 'windows-endpoints.json'
Write-Host "Recording for $Seconds seconds. Return to your game and play normally."
Write-Host 'Keep the vibration test panel CLOSED. This recorder does not generate vibration.'
try {
    # Separate job: unreachable device diagnostics never blocks host sampling.
    $job = Start-Job -ArgumentList $Address,$Seconds,$folder -ScriptBlock {
        param($address,$seconds,$folder)
        $ErrorActionPreference = 'Stop'
        $writer = New-Object System.IO.StreamWriter((Join-Path $folder 'switch.jsonl'),$false,
            (New-Object System.Text.UTF8Encoding($false)))
        $writer.AutoFlush = $true
        $udp = New-Object System.Net.Sockets.UdpClient
        $udp.Client.ReceiveTimeout = 300
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $after = [uint64]0
        $baseline = $true
        $nextStats = 0
        $nextHealth = 0
        $useHttp = $false
        $timeouts = 0
        function Save($row) {
            $row['collector_elapsed_ms'] = $clock.ElapsedMilliseconds
            $row['collector_wall_utc'] = [DateTime]::UtcNow.ToString('o')
            $writer.WriteLine(($row | ConvertTo-Json -Compress -Depth 12))
        }
        function ReadHttp([string]$path) {
            $request = [Net.HttpWebRequest]::Create("http://${address}:28773$path")
            $request.Proxy = $null
            $request.Timeout = 500
            $request.ReadWriteTimeout = 500
            $response = $request.GetResponse()
            $reader = New-Object IO.StreamReader($response.GetResponseStream())
            try { $reader.ReadToEnd() }
            finally { $reader.Dispose(); $response.Dispose() }
        }
        try {
            $udp.Connect($address,28772)
            Save @{stage='capture_begin'; address=$address; port=28772; read_only=$true}
            while ($clock.Elapsed.TotalSeconds -lt $seconds) {
                if ($clock.ElapsedMilliseconds -ge $nextHealth) {
                    $nextHealth = $clock.ElapsedMilliseconds + 5000
                    try {
                        $health = ReadHttp '/health'
                        $decodedHealth = $health | ConvertFrom-Json
                        if ($decodedHealth.stage -eq 'diagnostic_health' -and $decodedHealth.diagnostics) {
                            $useHttp = $true
                            Save @{stage='device_health'; reply=$health; transport='http'}
                        }
                    } catch {
                        Save @{stage='device_health_unavailable'; error=$_.Exception.Message}
                    }
                }
                $stats = $clock.ElapsedMilliseconds -ge $nextStats
                $command = if ($stats) { 'haptics' } else { "haptic-trace $after" }
                if ($stats) { $nextStats = $clock.ElapsedMilliseconds + 1000 }
                $bytes = [Text.Encoding]::ASCII.GetBytes($command)
                $transport = if ($useHttp) { 'http' } else { 'udp' }
                $pageSize = if ($useHttp) { 16 } else { 6 }
                try {
                    if ($useHttp) {
                        $path = if ($stats) { '/haptics' } else { "/haptic-trace?after=$after" }
                        $reply = ReadHttp $path
                    } else {
                        [void]$udp.Send($bytes,$bytes.Length)
                        $peer = New-Object Net.IPEndPoint([Net.IPAddress]::Any,0)
                        $reply = [Text.Encoding]::UTF8.GetString($udp.Receive([ref]$peer))
                    }
                    Save @{stage='device_reply'; request=$command; reply=$reply;
                        baseline_backlog=$baseline; transport=$transport}
                    $records = 0
                    foreach ($line in ($reply -split "`n")) {
                        if (-not $line.Trim()) { continue }
                        try { $row = $line | ConvertFrom-Json } catch { continue }
                        if ($null -ne $row.serial) {
                            $serial = [uint64]$row.serial
                            if ($serial -gt $after) {
                                if (-not $baseline -and $serial -ne ($after+1)) {
                                    Save @{stage='capture_gap'; after=$after; next=$serial}
                                }
                                $after = $serial
                            }
                            $records++
                        }
                    }
                    # Drain old ring contents before attributing packets to this run.
                    $traceResponse = $reply.Trim() -eq '{}' -or $records -gt 0
                    if (-not $stats -and $traceResponse -and $records -lt $pageSize -and $baseline) {
                        $baseline = $false
                        Save @{stage='trace_baseline'; after_serial=$after}
                    }
                    if ($records -lt $pageSize) { Start-Sleep -Milliseconds 20 }
                } catch {
                    $timeouts++
                    if ($useHttp) { $useHttp = $false }
                    if ($timeouts -eq 1 -or $timeouts % 20 -eq 0) {
                        Save @{stage='device_unavailable'; request=$command; error=$_.Exception.Message;
                            client_running_state='unknown'; transport=$transport}
                    }
                    Start-Sleep -Milliseconds 50
                }
            }
            Save @{stage='capture_end'; last_serial=$after; errors=$timeouts; baseline_completed=(-not $baseline)}
        } catch { Save @{stage='collector_error'; error=$_.Exception.Message} }
        finally { $udp.Close(); $writer.Dispose() }
    }
    $process = Start-Process -FilePath (Join-Path $PSScriptRoot 'steam-game-haptics-watch.exe') `
        -ArgumentList "--seconds $Seconds" -WorkingDirectory $PSScriptRoot -PassThru `
        -RedirectStandardOutput (Join-Path $folder 'host.jsonl') `
        -RedirectStandardError (Join-Path $folder 'host-errors.txt')
    try {
        $endpointJob = Start-Job -FilePath (Join-Path $PSScriptRoot 'read-windows-controller-endpoints.ps1') `
            -ArgumentList $endpointFile
    } catch { $issues.Add(($_ | Out-String)) }
    while (-not $process.HasExited -or $job.State -eq 'Running' -or $job.State -eq 'NotStarted') {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
    }
    @{ started=$started.ToString('o'); exit_code=$process.ExitCode; seconds=$Seconds; address=$Address } |
        ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $folder 'launch.json')
} catch { $issues.Add(($_ | Out-String)) }
finally {
    if ($endpointJob) {
        Remove-Job $endpointJob -Force
    }
    if (-not (Test-Path -LiteralPath $endpointFile)) {
        @{status='unavailable'; error='Windows device inventory did not produce a snapshot.'} |
            ConvertTo-Json | Set-Content -LiteralPath $endpointFile -Encoding UTF8
    }
    if ($job) {
        if ($job.State -eq 'Running' -or $job.State -eq 'NotStarted') { Stop-Job $job }
        Receive-Job $job -ErrorAction SilentlyContinue | Out-File (Join-Path $folder 'collector-output.txt')
        if ($job.ChildJobs[0].JobStateInfo.Reason) { $issues.Add($job.ChildJobs[0].JobStateInfo.Reason.ToString()) }
        Remove-Job $job -Force
    }
    if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        foreach ($name in @('controller.txt','console_log.txt','streaming_log.txt')) {
            try {
                $path = Join-Path $steam "logs\$name"
                if (Test-Path $path) { Get-Content $path -Tail 700 | Set-Content -Encoding UTF8 (Join-Path $folder $name) }
            } catch { $issues.Add(($_ | Out-String)) }
        }
    }
    try {
        $rows = @(Get-Content (Join-Path $folder 'host.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
        $states = @($rows | Where-Object { $_.stage -in @('host_state','sony_host_state') })
        $summary = @('只读游戏采集；记录器没有调用震动 API、修改触觉偏好或设备能力。',
            'host.jsonl：Steam 活动 AppID、普通力度缓存、原生 producer 的时间戳与缓存。',
            'sony_host_state：Steam 识别的 Sony 类型、能力、固件原始值和扳机效果缓存。',
            'windows-endpoints.json：Windows 当前 Sony/Valve 设备和关联音频输出，使用设备容器匹配。',
            '单独采集的 Windows 系统设备快照无法观察游戏进程内的 Steam Input 模拟或音频调用。',
            'switch.jsonl：Switch 诊断接口的原始回包；超时只代表采集不可达。',
            '主机状态变化是发送分支的采样证据，不是设备收包证明；不能由此识别游戏原始 API。',
            'native_producer_changed 包括停止请求，缓存的旧脉冲字不能单独证明新增非零脉冲。',
            'Switch kind=1/rc=0 是接收接受记录；kind=2/rc=0 是系统提交成功，仍不是机械测量。',
            'source=0 普通双路，1 Nintendo 原生，2 扳机，3 Sony 控制，6 专用 PCM。',
            '普通包不能反推出 XInput 或 Steam Input；Sony 控制包不能当成 DualSense 音频波形。',
            'baseline_backlog=true 的回包来自初始追赶，不据此归因本轮游戏；capture_gap 表示证据缺口。',
            '没有事件/版本不支持/诊断不可达，都不能证明游戏不支持高级触觉。', '')
        foreach ($group in ($states | Group-Object appid)) {
            $native = @($group.Group | Where-Object { $_.native_producer_changed }).Count
            $ordinary = @($group.Group | Where-Object { $_.ordinary_cache_changed }).Count
            $summary += "活动 AppID=$($group.Name)：普通缓存变化观测 $ordinary 次，原生 producer 变化观测 $native 次。"
            foreach ($sony in ($group.Group | Where-Object { $_.stage -eq 'sony_host_state' } |
                    Group-Object controller_type, sony_firmware_raw, device_caps)) {
                $first = $sony.Group[0]
                $changed = @($sony.Group | Where-Object { $_.sony_trigger_cache_changed }).Count
                $summary += ('Steam 类型 {0}，设备能力 {1}，固件原始值 0x{2:x4}，扳机缓存变化观测 {3} 次。' -f
                    $first.controller_type,$first.device_caps,[int]$first.sony_firmware_raw,$changed)
            }
        }
        if (-not $states.Count) { $summary += '没有读到有效主机状态；需检查 host.jsonl，不能判断游戏输出分支。' }
        $summary | Set-Content -Encoding UTF8 (Join-Path $folder '采集说明.txt')
    } catch { $issues.Add(($_ | Out-String)) }
    @{ read_only=$true; errors=@($issues.ToArray()); physical_fidelity_verified=$false } |
        ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $folder 'export.json')
    if (Test-Path (Join-Path $PSScriptRoot 'manifest.json')) {
        Copy-Item (Join-Path $PSScriptRoot 'manifest.json') $folder
    }
    $archive = Join-Path $root "game-results-$stamp.zip"
    Compress-Archive -Path (Join-Path $folder '*') -DestinationPath $archive -Force
    try {
        $beside = Join-Path $PSScriptRoot "game-results-$stamp.zip"
        Copy-Item -LiteralPath $archive -Destination $beside -Force
        $archive = $beside
    } catch { Write-Host "Exported to $archive" }
    Write-Host "Result archive: $archive"
    Start-Process explorer.exe -ArgumentList ('/select,"' + $archive + '"')
}
