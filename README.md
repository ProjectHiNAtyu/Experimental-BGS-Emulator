# bgs_emu

An experimental in-process BGS (Battle.net "Aurora" RPC) server for Blizzard-stack game
clients. It stands up a Windows SChannel TLS endpoint on fake sockets, speaks
RFC-6455 WebSocket, and answers the Aurora bootstrap + login + account +
session RPCs, plus a local resource download used during client startup.
The emulator supplies native protobuf responses so an integrating client can
complete its BGS startup flow.

One shared transport/protocol emulator. It includes the generic OS-side
plumbing (fake Winsock routing and the in-process TLS endpoint); the **host project owns all game-build-specific integration**
— the client-side TLS/pin/bundle/CN hooks and identity values a protected
retail client needs to accept the emulated server. See
[Client integration hooks](#client-integration-hooks).

## What's in the box

```
source/dll/
  main.cpp               DLL entry + bootstrap worker
  component/
    logger.cpp/hpp       File logger (bgs_emu.log, written to the process
                         current working directory)
    net.cpp/hpp          Winsock detours; routes BGS hosts to per-connection
                         in-process server sessions; gateway-target matcher
  loader/                Tiny component system (pre_start / early_post_unpack / pre_destroy)
  server/
    bgs_server.hpp       Aurora RPC dispatch: Connect, Logon, Subscribe,
                         GetAccountState / GetGameAccountState / GetLicenses,
                         CreateSession / GetSignedSessionState, GetContentHandle,
                         OnLogonComplete, OnSessionCreated, OnAccountStateUpdated, ...
    schannel_server.hpp  Server-side TLS via Windows SChannel
    tls_identity.hpp     Shared process certificate used by server-side TLS
    sink_server.hpp      Plain-HTTP fingerprint bundle and local resource responses
    local_resources.hpp  BN/apft WordFilters protobuf, download URL and SHA-256
    tcp_server.hpp/cpp   Async in-process TCP endpoint w/ worker thread
    base_server.hpp/cpp  Synthetic address (Jenkins hash of hostname) for routing
    registry.hpp         Lookup by address or by name
    protobuf.hpp/cpp     Minimal protobuf writer (varint / length-delimited / fixed32/64)
source/shared/utilities/ MinHook detour wrapper, allocator, hash, concurrency helper
deps/                    minhook, picoproto (included source)
```

## Build

Requires Visual Studio with the C++ x64 toolchain and a Windows SDK.
Place a Premake 5 executable at `tools/premake5.exe` before running
`generate.bat`. The `tools/` folder is local tooling and is excluded from Git.

```
generate.bat
build\bgs_emu.sln       # VS2022, x64, Debug or Release
```

Output: `build/bin/x64/<cfg>/bgs_emu.dll`.

The DLL exports:

- `bgs_emu_start()` — explicit "start now" entry point. Call it **outside
  `DllMain`** (e.g. from a worker thread or your host's init path), after
  `LoadLibrary` and before the game is allowed to do any networking. It is
  synchronous and serialized: if the fallback bootstrap is already running,
  the caller waits for it to finish rather than returning early.
- `bgs_emu_present()` — cheap sentinel for the host to confirm we loaded.
- `bgs_emu_configure_identity(account_id, game_account_id, display_name)` —
  copies the host's local identity for new BGS connections.

If the host loads us with a plain `LoadLibraryA` and does nothing else, the
bootstrap worker fires startup on its own ~50 ms after `DllMain`. That is a
**fallback, not a synchronization barrier** — always prefer the explicit
call.

## Configure the endpoints

Open `source/dll/component/net.cpp`. The top of the file has the hostname
tables:

```cpp
static const host_entry g_bgs_gateways_arr[] = {
    { "fakebgs.com",          1119 },
    { "us.actual.battle.net", 1119 },
    { "eu.actual.battle.net", 1119 },
    { "kr.actual.battle.net", 1119 },
    { "cn.actual.battle.net", 1119 },
};

static const host_entry g_cert_pin_sinks[] = {
    { "nydus.battle.net", 9090 },
};
```

- Names in `g_bgs_gateways_arr` resolving with service `1119` route to a
  **per-connection** `bgs_server` session (own SChannel context + queues).
  Any other connect to **port 1119** also falls through to the gateway.
- `g_cert_pin_sinks` entries answer plain HTTP on the given port. The
  fingerprint route returns an empty cert-pin bundle, and the local resource
  path returns the compressed word-filter data. If you change the resource
  hostname or port, update `local_resources.hpp` and the routing table together.

## Configure the identity

Open `source/dll/server/bgs_server.hpp`. The `bgs_config` struct at the top
has the placeholders every integrator will need to override per title:

Hosts can supply their existing local identity through
`bool bgs_emu_configure_identity(uint32_t account_id, uint32_t game_account_id,
const char* display_name)`. Resolve it with `GetProcAddress` and call it
outside `DllMain`, before the first BGS connection. Both IDs must be nonzero;
the name must contain 1–31 bytes. The function copies its arguments and
returns false for invalid input. New connections receive a snapshot;
existing connections retain their identity. Use the same account ID and
name that the downstream Demonware emulator returns.

```cpp
std::string display_name    = "Player";
uint32_t    program_fourcc  = 0x00000000u;   // per-title "program" value, sent as protobuf fixed32
uint32_t    region          = 1;
uint32_t    license_id      = 1;             // AccountLicense.id the client treats as "owned"
uint64_t    account_id      = 0xBEEF0001ull;
uint64_t    game_account_id = 0xBEEF0002ull;
std::string session_id      = "bgsemu-session-0001";
std::string session_token   = "bgsemu-session-token-placeholder";
std::string client_id       = "bgsemu-client-0001";
uint32_t    account_token_protocol = 0;
std::string web_credentials = "bgsemu-local-web-token";
```

`program_fourcc` identifies the client's program namespace. A four-character
`Logon.program` overrides this source default for that connection, as described
below. `license_id` must match the license expected by the integrating client;
the default `1` is a placeholder. `session_token` is an opaque local token —
configure it for the downstream authentication service you use.

The config is shared: `net.cpp` holds one `g_bgs_config` used for the
routing template and every per-connection session. Set the `bgs_config`
defaults, or initialize `g_bgs_config` in
`net::component::early_post_unpack()` before creating the routing templates
and installing the network hooks. The identity export updates account ID,
game-account ID, and display name; other values remain source settings.

## BGS program namespaces

A program identifier selects the namespace used in BGS account and session
messages. The same DLL handles these identifiers through the shared Winsock,
SChannel, WebSocket and Aurora code. A tag alone does not establish support
for a client's protocol or build; test its native startup flow separately.

### Where to set the program

Open [source/dll/server/bgs_server.hpp](source/dll/server/bgs_server.hpp).
At the top of the file, `bgsemu::bgs_config::program_fourcc` is the source
default. For example, use one of these numeric values as its initializer:

```cpp
uint32_t program_fourcc = 0x4F44494Eu;
```

Use `0x5A455553u` for the `ZEUS` namespace instead. Rebuild `bgs_emu.dll`
after changing source settings and replace the DLL beside your host.

Normally this field can remain `0`: the `AuthenticationServer.Logon`
handler in the same file reads `LogonRequest.program` from field 1. When
the client sends exactly four bytes, it packs them most significant byte
first and replaces the default **for that connection only**.

The values below are namespace identifiers, not a compatibility list:

| Program string | Numeric tag  |
| -------------- | ------------ |
| `ODIN`         | `0x4F44494E` |
| `ZEUS`         | `0x5A455553` |
| `CODC`         | `0x434F4443` |
| `AUKS`         | `0x41554B53` |
| `FORE`         | `0x464F5245` |
| `VIPR`         | `0x56495052` |
| `MW2R`         | `0x4D573252` |
| `WoW `         | `0x576F5720` |
| `D3  `         | `0x44332020` |
| `Fenr`         | `0x46656E72` |
| `OSI `         | `0x4F534920` |
| `ANON`         | `0x414E4F4E` |
| `SC2 `         | `0x53433220` |
| `SCR `         | `0x53435220` |
| `WTCG`         | `0x57544347` |
| `Hero`         | `0x4865726F` |
| `Pro `         | `0x50726F20` |
| `DST2`         | `0x44535432` |
| `STAR`         | `0x53544152` |
| `SEXP`         | `0x53455850` |
| `W2BN`         | `0x5732424E` |

### Related settings to check

These fields also live in `bgsemu::bgs_config` in
`source/dll/server/bgs_server.hpp`:

- `region`: the region included in game-account identity and license replies.
- `license_id`: the account license expected by the integrating client;
  the default `1` is a placeholder.
- `account_token_protocol`: the downstream account-token header's protocol
  value. It is separate from `program_fourcc`. The current Logon handler
  supplies `965353976` for `ODIN` when this field is zero. For another token
  format, set the correct source default or add a program-specific branch
  beside that `ODIN` branch in the handler.
- `session_token` and `web_credentials`: local credentials required by your
  downstream service and host's launcher-token adapter.

The shared configuration object is `net::g_bgs_config` in
[source/dll/component/net.cpp](source/dll/component/net.cpp). Set source
defaults in `bgs_config`, or initialize that object in
`net::component::early_post_unpack()` before the network hooks are installed.
Each new connection copies the configuration. The identity export changes
only account ID, game-account ID and display name; it does not set the
program, region, license or account-token protocol.

Check `bgs_emu.log` for `Logon title program=0x4F44494E` or
`Logon title program=0x5A455553` to verify what the client supplied. If that
line is absent, inspect the client's Logon field and the source default.
Program selection needs no extra client detour. Keep build-specific hook
addresses and signatures in the host project as described below.

## How to integrate

Load `bgs_emu.dll` early, then call `bgs_emu_start()` **outside `DllMain`**
and before the game is allowed to do any networking:

```cpp
if (HMODULE h = LoadLibraryA("bgs_emu.dll"))
{
    using start_fn = void (*)();
    const auto start = reinterpret_cast<start_fn>(
        GetProcAddress(h, "bgs_emu_start"));
    if (start) start();
}
```

### Routing the game's BGS traffic to `bgs_emu`

The emulator already hooks Winsock for its configured hostnames. If your host
has its own DNS import hook, this adapter can route the BGS gateways to the
local endpoint before forwarding other names to Winsock:

```cpp
static const char* bgs_emu_hosts[] = {
    "us.actual.battle.net",
    "eu.actual.battle.net",
    "kr.actual.battle.net",
    "cn.actual.battle.net",
};

int WSAAPI getaddrinfo_stub(const char* name, const char* service,
                            const addrinfo* hints, addrinfo** res)
{
    if (name && res)
    {
        for (const char* h : bgs_emu_hosts)
        {
            if (!_stricmp(name, h))
            {
                auto* in = utilities::memory::get_allocator()->allocate<sockaddr_in>();
                auto* ai = utilities::memory::get_allocator()->allocate<addrinfo>();
                *in = {};
                *ai = {};

                in->sin_family      = AF_INET;
                in->sin_addr.s_addr = inet_addr("127.0.0.1");
                in->sin_port        = htons(1119);

                ai->ai_family    = AF_INET;
                ai->ai_socktype  = SOCK_STREAM;
                ai->ai_protocol  = IPPROTO_TCP;
                ai->ai_addr      = reinterpret_cast<sockaddr*>(in);
                ai->ai_addrlen   = sizeof(*in);
                ai->ai_next      = nullptr;
                ai->ai_flags     = 0;
                ai->ai_canonname = nullptr;

                *res = ai;
                return 0;
            }
        }
    }

    return getaddrinfo(name, service, hints, res);
}
```

## Client integration hooks

The host owns integration that depends on the client build. Verify the target
address, argument layout, and success value for each build, and keep only the
hooks it needs. There is no universal address list or shared detour ABI.

| Integration point               | When the host needs it                                                                                    |
| ------------------------------- | --------------------------------------------------------------------------------------------------------- |
| Connection endpoint             | The client must be directed to the local BGS gateway.                                                     |
| Launcher setting/token          | The native logon flow expects a launcher-provided token.                                                  |
| Certificate verdict             | The client performs application-level certificate validation.                                             |
| SPKI/public-key pin             | The client compares the local certificate against an embedded pin.                                        |
| Bundle signature and acceptance | The client verifies or imports the cert-pin fingerprint bundle.                                           |
| CN/hostname check               | The client validates the certificate name independently of SChannel.                                      |
| Package ownership               | A separate client-specific ownership check requires integration; this is not a universal BGS requirement. |

Return values differ between SDK versions: a certificate verdict may use `0`
for success while another verifier returns `true`. Check the native caller
before installing a hook. Bundle integration must preserve the state the SDK
needs later in startup.

Account/session identity, resource replies, SHA-256 hashes, and resource
compression belong in the emulator. The latest resource fix required no new
client hook. Temporary RPC-decoder tracing was removed after verification;
native decoding remains in use.

### Example client-side detours

These client function hooks live in your host project, the project that loads
`bgs_emu.dll`. The emulator provides the server and transport surface.

The following stubs illustrate one possible set of signatures and success
values. Verify them against your client build before installing the detours;
keep only the hooks that build needs. Package ownership is optional, and
bundle acceptance may need to preserve native SDK state rather than simply
returning success.

```cpp
namespace client_hooks
{
    namespace bgs
    {
        utilities::hook::detour cert_verdict_hook;
        __int64 cert_verdict_stub(__int64 /*conn*/) { return 0; }

        utilities::hook::detour pin_verify_hook;
        __int64 pin_verify_stub(__int64* /*a1*/, int /*a2*/, __int64 /*pinned*/) { return 0; }

        utilities::hook::detour bundle_sig_hook;
        char bundle_sig_stub(void*, void*, const char* /*name*/) { return 1; }

        utilities::hook::detour check_cert_bundle_hook;
        char check_cert_bundle_stub(__int64 /*a1*/, __int64 /*a2*/) { return 1; }

        utilities::hook::detour cn_check_hook;
        bool cn_check_stub(uint64_t*, uint64_t* /*cn*/, uint64_t* /*host*/) { return true; }

        utilities::hook::detour pkg_owned_hook;
        char pkg_owned_stub(__int64 /*guid*/) { return 1; }
    }
}
```

## Protocol notes

### Hostname → server routing

Each `base_server` hashes its name through Jenkins-one-at-a-time and uses
that as a synthetic IPv4 address. The Winsock detours hand that address back
from `getaddrinfo`/`gethostbyname` for intercepted names, and `connect` maps
the resulting socket to the server by address or (as fallback for port 1119)
by name. `send`/`recv` run through the server's in-memory queues; the host's
real Winsock stack never sees any of it.

### TLS

`schannel_server` builds a self-signed cert with
`CN=cn.actual.battle.net` and runs the full handshake through Windows
SChannel server-side. Application-level pin and bundle checks require the host integration
described above; the local certificate is not in the client's trust store.

### WebSocket upgrade

The client sends `GET / HTTP/1.1 Upgrade: websocket
Sec-WebSocket-Protocol: v1.rpc.battle.net`. We answer `101 Switching
Protocols` with the RFC-6455 `Sec-WebSocket-Accept` (SHA-1 of key + the
RFC magic, base64).

### Aurora frames

Each WebSocket binary message carries:

```
[uint16 header_len BE] [Header protobuf] [payload protobuf]
```

Header fields: `service_id=1`, `method_id=2`, `token=3`, `size=5`,
`status=6`, `service_hash=11` (fixed32). A response uses `service_id=0xFE`
and echoes the request token. The server addresses server-initiated calls
(OnLogonComplete, OnSessionCreated, OnAccountStateUpdated)
by `service_hash` with `service_id=0` ("bindless").

### Login flow produced by the server

1. **ConnectionService.Connect (1)** → `ConnectResponse{ server_id, client_id, bind_result=0, server_time, use_bindless_rpc }`.
2. **AuthenticationServer.Logon (1)** → empty `NoData`, then:
3. **→ AuthenticationClient.OnLogonComplete (5)** with `LogonResult{ error_code=0, account_id, game_account_id, battle_tag, connected_region, session_key, client_id }`.
4. **AccountService.Subscribe (25)** → echo subscriber refs, assign subscriber ids on empty ones.
5. **→ AccountNotify.OnAccountStateUpdated (1) / OnGameAccountStateUpdated (2)** carrying the matching account or game-account state, subscriber ID, and subscription-completed flag.
6. **AccountService.GetSignedAccountState (44)** → opaque base64 blob carrying a 184-byte local record with a protocol value, account ID, and display name.
7. **AccountService.GetAccountState (30) / GetGameAccountState (31) / GetLicenses (32) / GetGameTimeRemainingInfo (33)** — direct responses from the configured identity.
8. **SessionService.CreateSession (1)** → `CreateSessionResponse{ session_id }`, then:
9. **→ SessionNotify.OnSessionCreated (1)** with `SessionCreatedNotification{ identity{ account, game_account }, reason=0, session_id, session_key, client_id }`.
10. **SessionService.GetSignedSessionState (10)** → `{ token = session_token }` (opaque — the client typically forwards this to its downstream Demonware auth leg).
11. **KeepAlive / MarkSessionsAlive / UpdateSession / DestroySession** — empty acks.
12. **AuthenticationServer.GenerateWebCredentials (8)** → `{ web_credentials = configured local credential bytes }`.

## Dependencies

- [minhook](https://github.com/TsudaKageyu/minhook) — the hook engine used for the Winsock detours.
- [picoproto](https://github.com/petewarden/picoproto) — protobuf reader (we ship our own minimal writer).

Their source and license files are included under `deps/`; no submodule
checkout is required.

## License

GNU Lesser General Public License v3.0 — see [LICENSE](LICENSE) (which
references [LICENSE.GPL](LICENSE.GPL) as GPLv3 is incorporated by
reference). The LGPL lets you link the DLL into a closed-source host
without that host becoming LGPL; modifications to the DLL itself still
flow back under the LGPL. Dependencies retain their own licenses under
`deps/*/LICENSE*`.
