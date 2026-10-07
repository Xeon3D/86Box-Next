# 86Box-Next ISP (isp-server)

The virtual dial-up ISP and telephone exchange for
[86Box-Next](https://github.com/Xeon3D/86Box-Next)'s emulated modems. A guest
dials in over its modem and gets PPP, an address, DNS and NAT to the
Internet; modems on the "Telephone network" line get phone numbers and can
call each other. It runs on its own: on the same PC as the emulator, or
hosted somewhere others can dial.

**No support, AI coded:** like the rest of 86Box-Next, this is written with
AI help and comes as it is.

## What it speaks

- **PPP** (RFC 1661/1662), passive until the guest starts it; IPCP with the
  guest's address, DNS and **WINS** servers (RFC 1877).
- **Authentication**: none, anyone (any name and password), or **accounts**
  only. Methods: **PAP**, **CHAP-MD5**, **MS-CHAP**, **MS-CHAP-2**
  (with the authenticator response), and **CHAP with SHA-1, SHA-256,
  SHA-384, SHA-512, SHA3-256, SHA3-384, SHA3-512** (IANA algorithms 6-12).
  The strongest allowed method is asked for first; the guest may name
  another allowed one.
- **Encryption**: **MPPE** 40-, 56- and 128-bit, stateless or stateful, with
  keys from MS-CHAP or MS-CHAP-2 (RFC 3078/3079); off, allowed, or required.
- **Compression (CCP)**: **MPPC** (alone or under MPPE), **Deflate**,
  **BSD-Compress** and **Predictor-1**.
- **Multilink** (RFC 1990): a guest's calls with the same name (and endpoint
  discriminator) make one bundle; packets are reassembled across its links
  and sent whole or in fragments over all of them.
- NAT per call by libslirp, guests reaching each other (guest LAN), port
  forwards, an optional modem-speed throttle.

Tested against Linux's own PPP (pppd and the kernel's MPPE, Deflate,
BSD-Compress and Multilink): `tests/pppd_interop.sh`.

## The status page

`http://<host>:2324/`, in tabs: **Status** (calls: who, how authenticated,
compression and encryption, Multilink), **Phone** (a phone in the browser,
the exchange's lines and calls), **Port forwards**, **Settings** (network,
who gets in and the dial-in accounts, encryption, compression), **Log**, and
**Users**.

**Users**: with none, the page is for this computer only (`127.0.0.1` or
`localhost`). The first user is the **super admin**, made on the Users tab
(from this computer) or, from elsewhere, with the setup token isp-server puts
in its log; from then on everyone logs in. The super admin adds **admins**
(everything but users) and **viewers** (status and log only).

## Running it

- **Windows**: `isp-server.exe` comes with 86Box-Next's releases. It is a
  window with the log that minimizes to the notification area.
- **Linux**: a static binary, no libraries needed:
  `./isp-server --help`.
- **macOS**: `isp-server` with its libraries beside it, Apple Silicon or
  Intel (built by the repository's `isp-server` workflow).
- **Docker** (`xeon3d/86box-next-isp`, amd64 and arm64):

  ```sh
  docker run -d --name isp -p 2323:2323 -p 2324:2324 -v isp-data:/data xeon3d/86box-next-isp
  docker logs isp        # the setup token for the first user
  ```

  Settings, accounts and users are kept in `/data/isp-server.ini`. Options
  may follow the image name, e.g. `--auth accounts --account alice:secret --mppe allowed`.

Point a modem at `<host>:2323`: its "Telephone network (isp-server)" line,
or a plain TCP line ("Dial the ISP"). Hosting it for others: use
`--auth accounts`, or anyone who can reach port 2323 gets on the Internet
through your host; put the page behind HTTPS (a reverse proxy that sets
`X-Forwarded-Proto: https`) so passwords do not cross the network in the
clear, and so the browser phone can use the microphone.

## Building

From the repository's root (it needs `src/char/modem_voice.c` beside it),
with CMake, pkg-config, libslirp, glib and zlib:

```sh
cmake -S isp-server -B build-isp -DBUILD_TESTING=ON
cmake --build build-isp
ctest --test-dir build-isp
```

`docker build -f isp-server/Dockerfile .` builds the container;
`--target linux-binary --output out` instead gives the static Linux binary.
