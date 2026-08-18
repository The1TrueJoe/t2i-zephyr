# Run as Administrator INSIDE the Windows VM. Windows PowerShell 5.1 compatible.
#
# Zadig's self-signed catalog is rejected with 0x800B0109 because Windows reports
# "Code Integrity is enforced". On ARM64 the Memory Integrity toggle is often hidden from
# Settings, so this reports what is actually enforcing before anything is changed.

function Name-Service($n) {
    switch ([int]$n) {
        1 { 'CredentialGuard' }
        2 { 'HVCI (Memory Integrity)' }
        3 { 'SystemGuard' }
        4 { 'SecureLaunch' }
        7 { 'SmartAppControl' }
        default { "service $n" }
    }
}

$dg = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
if ($dg) {
    Write-Host "VBS status         : $($dg.VirtualizationBasedSecurityStatus)  (0=off 1=enabled-not-running 2=running)"
    $run = @($dg.SecurityServicesRunning    | ForEach-Object { Name-Service $_ }) -join ', '
    $cfg = @($dg.SecurityServicesConfigured | ForEach-Object { Name-Service $_ }) -join ', '
    Write-Host "Services running   : $run"
    Write-Host "Services configured: $cfg"
} else {
    Write-Host "DeviceGuard WMI class not present (are you elevated?)."
}

$hvci = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
$hv = Get-ItemProperty $hvci -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "HVCI registry Enabled : $($hv.Enabled)"

try { $sb = Confirm-SecureBootUEFI } catch { $sb = 'n/a' }
Write-Host "Secure Boot enabled   : $sb"
Write-Host "Test signing          : $((bcdedit | Select-String 'testsigning') -join ' ')"

# Smart App Control: 1 = on/enforced. Turning it OFF is IRREVERSIBLE without reinstalling Windows.
$ci = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue
Write-Host "Smart App Control     : $($ci.VerifiedAndReputablePolicyState)  (1=on/enforced, 0=off, 2=evaluation)"

# BitLocker decides whether touching Secure Boot risks a recovery-key prompt.
Write-Host ""
$bl = Get-BitLockerVolume -ErrorAction SilentlyContinue
if ($bl) { $bl | Select-Object MountPoint, VolumeStatus, ProtectionStatus | Format-Table }
else { Write-Host "BitLocker: no volumes reported (not elevated, or feature absent)" }
