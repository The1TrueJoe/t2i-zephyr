# Run as Administrator INSIDE the Windows VM, after Zadig has bound WinUSB to the T2i.
#
# Zadig publishes a GUID of its own invention. Integration Designer auto-discovers the remote by
# opening RTI's specific WinUSB device interface, so without this the driver is installed, the
# device works, and ID still sees nothing. The GUID below is RTI's own, read out of
# rtiwinusb.inf [Dev_AddReg] in RTIUSB2_Install.exe.

$guid = '{b0b650d9-8169-4343-89df-ca55cef25059}'
$base = 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_13BD&PID_1028'

if (-not (Test-Path $base)) {
    throw "No T2i at VID_13BD&PID_1028. Connect it to this VM (Parallels > Devices > USB) first."
}

Get-ChildItem $base | ForEach-Object {
    $dp = Join-Path $_.PSPath 'Device Parameters'
    if (-not (Test-Path $dp)) { New-Item -Path $dp -Force | Out-Null }
    New-ItemProperty -Path $dp -Name DeviceInterfaceGUIDs `
        -PropertyType MultiString -Value @($guid) -Force | Out-Null
    Write-Host "set DeviceInterfaceGUIDs on $($_.PSChildName)"
}

Write-Host ""
Write-Host "Current binding:"
Get-PnpDevice -InstanceId 'USB\VID_13BD&PID_1028*' |
    Select-Object Status, Class, FriendlyName, InstanceId | Format-List

Write-Host "Now unplug and replug the remote, then start Integration Designer."
