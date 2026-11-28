# ADR 0276 — The machine's network addresses

- Status: **Accepted** 2026-11-28, for Marivil's CeeUI port (its phase 5, calls: the owner chose WebRTC in CeeSafe,
  as the MAUI app has its own in C#). Implemented the same day.
- Scope: `lib/std/net.cee` (`NetworkInterfaces`, `NetworkAddress`), `runtime/hosted/ceesafe_rt_io.c`
  (`ceesafe_net_addresses`), `runtime/include/ceesafe/rt.h`, `src/interpreter/io.cee`, type-system.md §40.4,
  `spec/api/std.md`, the run-pass test `network-addresses`.

## 1. Context

- ICE (RFC 8445 §5.1.1.1) offers each of the machine's addresses as a host candidate: two phones on one Wi-Fi network
  reach each other directly only through them. A socket bound to the wildcard address does not say which addresses
  it is reachable at, and lib/std had no way to ask.
- Every system CeeSafe targets has the call: `getifaddrs` (POSIX systems, iOS, Android from API level 24) and
  `GetAdaptersAddresses` (Windows).

## 2. Decision

```
for (a in NetworkInterfaces.addresses()?.view()) {
    if (a.up && !a.loopback) { offer(a.address); }      // SocketAddress, port 0
}
```

- **`NetworkInterfaces.addresses()`** gives `Result<Vector<NetworkAddress>, IoError>`: every IPv4 and IPv6 address
  of every interface, in the system's order. A `NetworkAddress` has `interfaceName`, `interfaceIndex`, `address` (a
  `SocketAddress` with port 0; an IPv6 link-local one has the interface's index as its scope id, so it can be bound
  as it is), `prefixLength` (from the netmask, or Windows' on-link prefix), `up` (up and running) and `loopback`.
- **The runtime** is one call, `int64_t ceesafe_net_addresses(uint8_t *out, uint64_t capacity)`: records written
  while they fit (family, flags, prefix length, the name's length, the index little-endian, the address, the name),
  and the bytes all of them need, or a negated errno. lib/std calls again with that capacity when it is more.
- **Windows** loads `GetAdaptersAddresses` from `iphlpapi.dll` at the first call (skipping anycast, multicast and DNS
  server entries), so no program, and no compiler's command line, needs another library. Its `AdapterName` (the
  adapter's GUID name) is the name, and `Ipv6IfIndex` the index of an IPv6 address.
- **Unsupported**: CosmoOS userland, and Android before API level 24 (asked at run time, as ADR 0188 §2.2 asks).
- **The interpreter** answers a fixed machine, so a run stays reproducible: `lo`, index 1, up and loopback, with
  `127.0.0.1/8` and `::1/128`.

## 3. Alternatives

- **The connected-UDP-socket trick** (connect to an outside address, read the local one): only the default route's
  address, none on a machine offline, and no IPv6 link-local ones.
- **Leaving it to each app's `extern "C"`**: every WebRTC or discovery user would repeat the four systems.

## 4. Testing

- `run-pass/network-addresses`: there is an address; 127.0.0.1/8 is on an interface that is up and loopback; every
  name is non-empty, every port 0 and every prefix within its family. It runs natively on every release target and
  in the interpreter.
