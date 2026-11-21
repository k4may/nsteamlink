param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'
$result = [ordered]@{
    started_utc = [DateTime]::UtcNow.ToString('o')
    status = 'unavailable'
    sony_nodes = @()
    valve_nodes = @()
    render_nodes_enumerated = 0
    render_container_unavailable = 0
    related_render_endpoints = @()
}

function Read-ContainerId($device) {
    try {
        $value = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_ContainerId).Data
        $id = [guid]$value
        if ($id -ne [guid]::Empty -and $id -ne [guid]'00000000-0000-0000-ffff-ffffffffffff') {
            $id.ToString()
        }
    } catch { return $null }
}

function Read-Device($device) {
    [ordered]@{
        instance_id = $device.InstanceId
        device_class = $device.Class
        name = $device.FriendlyName
        pnp_status = $device.Status
        container_id = Read-ContainerId $device
    }
}

try {
    $devices = @(Get-PnpDevice -PresentOnly)
    $result.sony_nodes = @($devices | Where-Object {
        $_.InstanceId -match 'VID_054C&PID_(0CE6|0DF2)(?:&|\\|$)'
    } | ForEach-Object { Read-Device $_ })
    $result.valve_nodes = @($devices | Where-Object {
        $_.InstanceId -match '^(HID|USB)\\VID_28DE&PID_'
    } | ForEach-Object { Read-Device $_ })
    $sonyContainers = @($result.sony_nodes | ForEach-Object { $_.container_id } | Where-Object { $_ })
    foreach ($device in $devices) {
        if ($device.Class -ne 'AudioEndpoint' -or
            $device.InstanceId -notmatch '^SWD\\MMDEVAPI\\\{0\.0\.0\.00000000\}\.(\{[a-f0-9-]+\})$') { continue }
        $endpointId = $Matches[1]
        $result.render_nodes_enumerated++
        $endpoint = Read-Device $device
        if (-not $endpoint.container_id) { $result.render_container_unavailable++ }
        $containerMatch = $endpoint.container_id -and $endpoint.container_id -in $sonyContainers
        if (-not ($containerMatch -or $endpoint.name -match 'DualSense|Wireless Controller|PlayStation')) { continue }
        $endpoint.sony_container_match = [bool]$containerMatch
        $endpoint.mmdevices_render_state = $null
        try {
            $endpoint.mmdevices_render_state = Get-ItemPropertyValue -Name DeviceState -LiteralPath (
                'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\' + $endpointId)
        } catch { $endpoint.mmdevices_state_error = $_.Exception.Message }
        $result.related_render_endpoints += $endpoint
    }
    $result.status = 'ok'
} catch {
    $result.error = $_.Exception.Message
}
$result.finished_utc = [DateTime]::UtcNow.ToString('o')
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
