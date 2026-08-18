# Run elevated. Answers one question: is WinUSB actually bound to the T2i, and does it
# publish RTI's interface GUID? "Status OK" alone does not tell you either.
$id = (Get-PnpDevice -InstanceId 'USB\VID_13BD&PID_1028*').InstanceId
Write-Host "InstanceId : $id"
foreach ($k in 'DEVPKEY_Device_Service','DEVPKEY_Device_DriverInfPath','DEVPKEY_Device_Class','DEVPKEY_Device_DriverProvider') {
    $v = (Get-PnpDeviceProperty -InstanceId $id -KeyName $k -ErrorAction SilentlyContinue).Data
    Write-Host ("{0,-30}: {1}" -f $k, $v)
}
$dp = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
Write-Host ""
Write-Host "DeviceInterfaceGUIDs: $((Get-ItemProperty $dp -ErrorAction SilentlyContinue).DeviceInterfaceGUIDs)"
