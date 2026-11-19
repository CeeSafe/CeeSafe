#!/usr/bin/env python3
"""Bluetooth printers (ADR 0177): the API against the runtime's script backend, on every system, with no radio.

One program is built, and run with CEESAFE_BLUETOOTH_SCRIPT naming a file of devices and CEESAFE_BLUETOOTH_DUMP a file
that receives what each link does:

1. the paired printers (Classic) and a scan's (Low Energy) are listed, in the script's order, a name with spaces and
   one with UTF-8 kept whole, an empty name allowed;
2. a link writes its bytes in order, in as many writes as the program makes, and its drop closes it;
3. a device that is not there is `NotFound` (a Classic address opened as Low Energy too), one that will not connect
   `Refused`, a link that breaks `Lost` once the bytes before the break are written;
4. `off` makes every call `Off`, `denied` every call `Denied`;
5. with no script on a system whose step has not come (ADR 0177 §5), and under `ceesafec run --interp`, every call
   is `Unsupported`.

usage: python3 tools/bluetooth_check.py --compiler out/ceesafec
"""

import argparse
import os
import subprocess
import sys
import tempfile

PROGRAM = r'''fn kind(e: BluetoothError) -> StringView => match (e) {
    BluetoothError.Unsupported => "unsupported",
    BluetoothError.Off => "off",
    BluetoothError.Denied => "denied",
    BluetoothError.NotFound => "not found",
    BluetoothError.Refused => "refused",
    BluetoothError.Lost => "lost",
    BluetoothError.Resources => "resources",
};

fn show(list: Result<Vector<BluetoothDevice>, BluetoothError>) {
    match (list) {
        Ok(devices) => {
            Console.writeLine(devices.length);
            for (d in devices.view()) {
                Console.write(if (d.transport == BluetoothTransport.Classic) "classic " else "le ");
                Console.write(d.address.view());
                Console.write("|");
                Console.write(d.name.view());
                Console.writeLine("|");
            }
        }
        Err(e) => { Console.writeLine(kind(e)); }
    }
}

/// Opens a device by its address and transport, and writes "AB", then "CDE".
fn print(address: StringView, transport: BluetoothTransport) {
    let device = BluetoothDevice { name: String.create(), address: String.from(address)!, transport: transport };
    match (BluetoothLink.open(device)) {
        Ok(l) => {
            var link = l;
            let first = [0x41u8, 0x42];
            let second = [0x43u8, 0x44, 0x45];
            match (link.write(first[0..])) {
                Ok() => {
                    match (link.write(second[0..])) {
                        Ok() => { Console.writeLine("printed"); }
                        Err(e) => { Console.writeLine(kind(e)); }
                    }
                }
                Err(e) => { Console.writeLine(kind(e)); }
            }
        }
        Err(e) => { Console.writeLine(kind(e)); }
    }
}

fn main() {
    show(Bluetooth.pairedPrinters());
    show(Bluetooth.scanPrinters(50));
    print("00:11:22:AA:BB:CC", BluetoothTransport.Classic);
    print("6E4F0000-AAAA", BluetoothTransport.LowEnergy);
    print("00:11:22:AA:BB:CC", BluetoothTransport.LowEnergy);
    print("00:11:22:AA:BB:DD", BluetoothTransport.Classic);
    print("00:11:22:AA:BB:EE", BluetoothTransport.Classic);
    print("99:99:99:99:99:99", BluetoothTransport.Classic);
}
'''

DEVICES = """paired 00:11:22:AA:BB:CC Van printer 58mm
paired 00:11:22:AA:BB:DD Spare
paired 00:11:22:AA:BB:EE طابعة
nearby 6E4F0000-AAAA BLE printer
nearby 6E4F0000-BBBB
refuse 00:11:22:AA:BB:DD
lose 00:11:22:AA:BB:EE 4
"""

EXPECTED = """3
classic 00:11:22:AA:BB:CC|Van printer 58mm|
classic 00:11:22:AA:BB:DD|Spare|
classic 00:11:22:AA:BB:EE|طابعة|
2
le 6E4F0000-AAAA|BLE printer|
le 6E4F0000-BBBB||
printed
printed
not found
refused
lost
not found
"""

EXPECTED_DUMP = """open 00:11:22:AA:BB:CC
write 4142
write 434445
close 00:11:22:AA:BB:CC
open 6E4F0000-AAAA
write 4142
write 434445
close 6E4F0000-AAAA
open 00:11:22:AA:BB:EE
write 4142
write 4344
close 00:11:22:AA:BB:EE
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    compiler = os.path.abspath(args.compiler)
    problems = []
    cases = 0
    with tempfile.TemporaryDirectory() as work:
        source = os.path.join(work, "main.cee")
        exe = os.path.join(work, "bt.exe" if os.name == "nt" else "bt")
        with open(source, "w", encoding="utf-8") as f:
            f.write(PROGRAM)
        built = subprocess.run([compiler, "build", source, "-o", exe], capture_output=True, text=True)
        if built.returncode != 0:
            print(f"FAIL build: {built.stderr.strip()[:800]}")
            print("bluetooth_check: 0 cases: 1 problems")
            return 1

        def run(script_text, expected, dump_expected=None, interp=False, label=""):
            nonlocal cases
            cases += 1
            env = dict(os.environ)
            env.pop("CEESAFE_BLUETOOTH_SCRIPT", None)
            env.pop("CEESAFE_BLUETOOTH_DUMP", None)
            dump = os.path.join(work, f"dump{cases}.txt")
            if script_text is not None:
                script = os.path.join(work, f"script{cases}.txt")
                with open(script, "w", encoding="utf-8") as f:
                    f.write(script_text)
                env["CEESAFE_BLUETOOTH_SCRIPT"] = script
                env["CEESAFE_BLUETOOTH_DUMP"] = dump
            command = [compiler, "run", "--interp", source] if interp else [exe]
            result = subprocess.run(command, capture_output=True, env=env)
            out = result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
            if result.returncode != 0:
                problems.append(f"{label}: exit {result.returncode}: {result.stderr.decode('utf-8', 'replace')[:400]}")
            elif out != expected:
                problems.append(f"{label}: printed\n{out}\nnot\n{expected}")
            if dump_expected is not None:
                text = ""
                if os.path.exists(dump):
                    with open(dump, encoding="utf-8") as f:
                        text = f.read()
                if text != dump_expected:
                    problems.append(f"{label}: the links did\n{text}\nnot\n{dump_expected}")

        run(DEVICES, EXPECTED, EXPECTED_DUMP, label="devices")
        run("off\n" + DEVICES, "off\noff\n" + "off\n" * 6, "", label="off")
        run("denied\n" + DEVICES, "denied\ndenied\n" + "denied\n" * 6, "", label="denied")
        run("", "0\n0\n" + "not found\n" * 6, "", label="empty")
        run(None, "unsupported\n" * 8, label="no script")
        run(DEVICES, "unsupported\n" * 8, interp=True, label="interpreter")
    for p in problems:
        print(f"FAIL {p}")
    print(f"bluetooth_check: {cases} cases: {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
