# Getting Integration Designer to see a T2i (Windows 11 on ARM)

**Working procedure, verified 2026-08-18.** Skip to §3 — the rest is why the obvious routes fail,
so nobody burns another evening on them.

## 1. What ID actually needs

The T2i is `USB\VID_13BD&PID_1028`. Its descriptor says `bDeviceClass = 0x02` (CDC), so macOS binds
it as `/dev/cu.usbmodem*` — but **Windows must bind it to WinUSB**, because Integration Designer
auto-discovers by opening a WinUSB device interface:

```
{b0b650d9-8169-4343-89df-ca55cef25059}
```

That GUID is RTI's own, from `rtiwinusb.inf [Dev_AddReg]` inside `RTIUSB2_Install.exe`. **No COM
port driver will ever work** — `usbser` gets you a working serial device that ID cannot see.

## 2. Why the obvious routes fail on ARM64

* **`RTIUSB2_Install.exe`** — declares only `NTx86, NTamd64, NTia64` and ships only `i386/`,
  `amd64/`, `x86/` binaries. Windows-on-ARM never emulates kernel drivers. Its catalog is also
  SHA-1 only (no SHA-256), which modern Windows rejects independently.
* **Zadig 2.9** — builds `installer_arm64.exe` and self-signs a catalog, but the install dies with
  `0x800B0109` ("root certificate which is not trusted"), because Windows ignores user-installed
  roots for driver signing. Verified this is **not** fixable here: HVCI off, Smart App Control off,
  Secure Boot off, `testsigning Yes` — and it still binds nothing. Do not retry it.

## 3. What works

1. **Hand the device to the VM.** Parallels → Devices → USB & Bluetooth → connect `T2i`. Confirm
   `/dev/cu.usbmodem*` **disappears** on the Mac — that is the proof it moved.
2. **Bind Windows' own inbox WinUSB.** Device Manager → the T2i → Update driver → Browse → Let me
   pick → untick *Show compatible hardware* → **Have Disk** → `C:\Windows\INF\winusb.inf` →
   **WinUSB Device**. Microsoft-signed and present on ARM64, so no signing problem at all.
   Confirm with [tools/windows/check-binding.ps1](../tools/windows/check-binding.ps1):
   `Service: WINUSB`, `DriverInfPath: winusb.inf`, `Provider: Microsoft`.
3. **Publish RTI's interface GUID** — the step that is easy to miss, because without it you have a
   perfectly working WinUSB device and ID still sees nothing:
   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass -Force; .\set-rti-guid.ps1
   ```
4. **Unplug/replug**, then start Integration Designer. Re-run `check-binding.ps1` first if you want
   to see `DeviceInterfaceGUIDs` populated.

Scripts live in [tools/windows/](../tools/windows/). Run them elevated; unelevated they silently
report nothing useful.

## 4. Undo the Secure Boot detour

Secure Boot was disabled and test signing enabled while chasing Zadig. **Neither is needed** by the
procedure above, so put them back:

```powershell
bcdedit /set testsigning off
```
then restore the Parallels VM config from the `config.pvs.presecureboot` backup (VM shut down).

Safe to do because BitLocker was `FullyDecrypted` — on an encrypted volume, changing Secure Boot
prompts for a recovery key. Check with `check-code-integrity.ps1` before touching it on any other
machine.
