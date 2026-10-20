# Changelog

What changed for people who use Sarke, newest first. The plan ahead is in
[docs/ROADMAP.md](docs/ROADMAP.md); how releases are made is in [docs/RELEASING.md](docs/RELEASING.md).

## Unreleased

- **Pairing and encryption (protocol 3, not compatible with 1.1).**
  - PCs pair once by comparing a 6-digit code on both screens.
  - From then on they recognise each other by key, and every channel (input, clipboard,
    arrangement, window pictures, files) is encrypted with TLS 1.3. A PC that isn't paired can't
    connect.
  - Window pictures and files belong to the connected PC's session: another device on the network
    can no longer take over the picture or drop files into `Downloads\KVM`.
  - Video and file ports changed only on the host now work: the client follows them.
  - The list of PCs knows each PC by its key. A PC that isn't paired shows "Pair…", and only paired
    PCs can be set to connect automatically. PCs set to Auto in 1.1 get Auto again once paired.
- Sarke is open source under the Apache License 2.0; the installer ships `LICENSE`, `NOTICE` and
  the licenses of everything compiled in (`licenses\` in the install folder).
- Installers are published as GitHub Releases (and on the website), no longer kept in the repository.
- For developers: CI on every push, tests under ctest (`ctest --preset unit` / `local`),
  warnings are errors, clang-format, the Virtual Display Driver fetched by `tools\fetch_vdd.ps1`.

## 1.1.0 (2026-10-18)

The hackathon release: KVM both ways, clipboard (text, images, files), file drag across the edge,
the window viewer, desk mode with the Virtual Display Driver, discovery of PCs on the network,
display arrangement shared by both PCs, and the one-file installer.
