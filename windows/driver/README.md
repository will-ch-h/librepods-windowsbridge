# Driver package for the installer

**CI does this for you.** `.github/workflows/release.yml` downloads the signed
`driver-latest` release from https://github.com/will-ch-h/l2cap-windowsdriver into this
folder before compiling `installer.iss` — nothing to do manually for a normal release
build.

For a **local** installer build, populate this folder yourself first with the same four
files (grab them from that repo's `driver-latest` release, or build + sign your own per
its BUILD.md):

- `BthEchoSampleCli.inf`
- `BthEchoSampleCli.sys`
- `kmdfsamples.cat`   (must be *signed*)
- `l2cap-bridge.cer`  (the public half of the signing cert)

`installer.iss` bundles whatever is in this folder and, on install, imports the cert into
the LocalMachine Root and TrustedPublisher stores and runs `pnputil /add-driver` against
the INF. The target machine still needs test-signing mode enabled first
(`bcdedit /set testsigning on`, reboot, Secure Boot off) — the installer does not do that
for you.

This folder's contents (other than this README) are gitignored: they're per-signer
binary/cert artifacts, not source.
