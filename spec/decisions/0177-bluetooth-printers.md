# ADR 0177 — Bluetooth printers: a byte stream to a paired or nearby device

- Status: **Accepted** by the owner, 2026-11-19 ("I accept ADR 0177 as written"), after choosing the systems
  ("Android, macOS, Windows, iOS") and saying the vans' printers speak both Classic Bluetooth and Bluetooth Low Energy.
  Not implemented.
- Scope: a new std module, `lib/std/bluetooth.cee` (type-system.md §26.19); the hosted runtime
  (`runtime/hosted/ceesafe_rt_bluetooth.c`) and Android's Java (`runtime/android/org/ceesafe/bluetooth/`); the
  diagnostics of nothing (no language change); the guide's page on the system's services; claywind-pack's manifests
  (ClayWind's repository: Android's permissions, iOS's usage sentence).

## 1. Context

- **The need.** IPos prints a receipt to a thermal printer in the van: `escPos` (ceesafe_reporting) makes the bytes,
  and nothing in CeeSafe can send them. The .NET app did it on Android only: the printer paired in the phone's
  settings, chosen by name, a Classic RFCOMM socket to the Serial Port Profile (`00001101-0000-1000-8000-00805F9B34FB`),
  the bytes written, the socket closed. Its iOS, Mac Catalyst and Windows services are stubs.
- **What a printer offers.** A Bluetooth ESC/POS printer is a byte sink. Over Classic Bluetooth it is a serial port
  (SPP, an RFCOMM channel). Over Bluetooth Low Energy it is a GATT characteristic that takes writes, in a vendor's
  service: `18F0`/`2AF1`, `FF00`/`FF02`, `E7810A71-73AE-499D-8C15-FAA9AEF0C3F2`/`BEF8D6C9-9C21-4C9E-B632-BD58C1009F9F`
  are the common ones. The vans' printers offer both.
- **What each system allows.** Android, macOS and Windows open RFCOMM to a paired device. iOS does not, outside
  Apple's MFi programme: on an iPhone a printer is reachable by BLE only. Android, macOS and Windows have BLE too, but
  RFCOMM to a paired printer is what users already set up, and it is faster.

## 2. Decision

### 2.1 The API

```ceesafe
public enum BluetoothTransport { Classic, LowEnergy }

public struct BluetoothDevice {
    public name: String;            // as the system shows it; may be empty for a BLE device that has none
    public address: String;         // the system's id: "00:11:22:AA:BB:CC", or on Apple's systems a UUID
    public transport: BluetoothTransport;
}

public enum BluetoothError { Unsupported, Off, Denied, NotFound, Refused, Lost, Resources }

public struct Bluetooth {
    /// The paired devices that offer a serial port (Classic), in the system's order. Android, macOS, Windows;
    /// `Unsupported` on iOS.
    public fn pairedPrinters() -> Result<Vector<BluetoothDevice>, BluetoothError>;
    /// The BLE devices in range that advertise one of the printer services above, found within `milliseconds`.
    /// Every system; on iOS the only way to find a printer.
    public fn scanPrinters(milliseconds: u32) -> Result<Vector<BluetoothDevice>, BluetoothError>;
}

/// A connection to a printer: bytes written in order, each `write` returning once the system has taken them.
public struct BluetoothLink {
    public fn open(device: borrow BluetoothDevice) -> Result<BluetoothLink, BluetoothError>;
    public fn write(borrow mut this, bytes: Slice<u8>) -> Result<void, BluetoothError>;
    // drop closes it
}
```

- **Blocking.** Every call blocks until done (a scan for its time, `open` until connected or refused, `write` until
  the bytes are taken) and may be made from any thread. A windowed program calls them off its UI thread, as it does
  network requests; the runtime never needs the window's run loop for them (macOS and iOS run their Bluetooth work on
  a private dispatch queue or run loop thread of the runtime's).
- **One kind of link.** `open` takes either transport. Classic: RFCOMM to the SPP service, its channel found by SDP.
  LowEnergy: connect, find the first characteristic of a known printer service that takes writes (with or without a
  response), and write in pieces of the largest length the connection allows (`maximumWriteValueLength`, the ATT MTU
  less 3), with a response when the characteristic offers it, so that a write's return means the printer has the
  bytes. Pieces without responses are paced by the system's flow control (iOS's `canSendWriteWithoutResponse`,
  Android's `onCharacteristicWrite`).
- **Errors.** `Unsupported`: the system has no such Bluetooth (iOS Classic; Linux and FreeBSD, deferred; a machine with
  no adapter). `Off`: the adapter is off. `Denied`: the user refused the permission. `NotFound`: the device is not
  paired or not in range. `Refused`: it would not connect (another phone holds it, it is not a printer). `Lost`: the
  link broke during a write. `Resources`: memory or handles ran out.

### 2.2 Permissions

- **Android 12 and later**: `BLUETOOTH_CONNECT` (and `BLUETOOTH_SCAN` with `neverForLocation` for a scan), asked at the
  first call that needs it, through the window's activity, the call waiting for the user's answer: `Denied` if refused.
  Android 11 and earlier: `BLUETOOTH` and `BLUETOOTH_ADMIN`, and for a scan `ACCESS_FINE_LOCATION`. claywind-pack's
  `--bluetooth WHY` writes them into the manifest.
- **iOS**: `NSBluetoothAlwaysUsageDescription`, WHY, in Info.plist (claywind-pack's `--bluetooth WHY`); the system asks
  at the first use. Without the key iOS ends the program, so the runtime checks for it and answers `Denied` instead.
- **macOS**: a bundled app needs `NSBluetoothAlwaysUsageDescription` too (claywind-pack); a program run from a terminal
  is under the terminal's permission.
- **Windows**: none.

### 2.3 Per system

| System | Paired printers | Scan | Classic link | BLE link |
|---|---|---|---|---|
| Android | `BluetoothAdapter.getBondedDevices`, those with the SPP UUID | `BluetoothLeScanner`, filtered by the services | `createRfcommSocketToServiceRecord(SPP)` | `BluetoothGatt` |
| macOS | `IOBluetoothDevice.pairedDevices`, those with an SPP record | CoreBluetooth | IOBluetooth `openRFCOMMChannelSync` on the SDP record's channel | CoreBluetooth |
| iOS | `Unsupported` | CoreBluetooth | `Unsupported` | CoreBluetooth |
| Windows | `BluetoothFindFirstDevice` (Bthprops), remembered devices | WinRT's `BluetoothLEAdvertisementWatcher` | Winsock `AF_BTH` with `SerialPortServiceClass_UUID` | WinRT GATT |
| Linux, FreeBSD | `Unsupported` (deferred) | `Unsupported` | `Unsupported` | `Unsupported` |

Apple's frameworks are loaded with dlopen and driven through the Objective-C runtime, as AppKit and UIKit are (no
program links a framework); Windows's Bthprops and Ws2_32 likewise; Android's through a class of the runtime's dex,
as the file picker is.

### 2.4 Testing

- A script backend, `CEESAFE_BLUETOOTH_SCRIPT=<file>`, as the window's: devices to list, and a file that receives what
  each link writes, so the API and IPos's printing are tested on every system with no radio.
- On devices, by the owner: a receipt printed on the van's printer from an Android phone, an iPhone, the Mac and the
  Windows laptop.

## 3. Options not taken

- **Only what the .NET app did** (Classic on Android). The owner chose all four systems.
- **A general Bluetooth API** (any GATT service, notifications, reads, a peripheral role). Larger, and nothing needs
  it yet; this API grows into it, `BluetoothLink` being the first use.
- **The system's print dialogs instead** (AirPrint, Android's print service, Windows's spooler). Thermal receipt
  printers are rarely there, and a receipt is ESC/POS, not a page. A page printer for the A4 invoice is another ADR.
- **Asynchronous calls** (an `async` API on the reactor). Bluetooth's callbacks arrive on the systems' own threads or
  queues; a blocking call on a worker is what IPos's requests already do, and simpler to make right on four systems.

## 4. Consequences

- IPos prints a receipt: a printer chosen once in Settings (the paired ones, then a scan), kept like the server's
  address; Print sends the receipt's ESC/POS on a worker thread.
- claywind-pack gains `--bluetooth WHY`.
- ClayWind's finding list (CEESAFE-FINDINGS) records the gap and this ADR.

## 5. Order of work

1. The std module, the script backend, the tests (every system).
2. macOS: Classic and BLE (the Mac here, the van's printer).
3. Android: Classic and BLE; claywind-pack's permissions.
4. iOS: BLE; claywind-pack's usage sentence.
5. Windows: Classic, then BLE.
6. IPos: the printer in Settings, Print on the receipt preview.
