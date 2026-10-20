# Releasing Sarke

Every release goes to two places: a **GitHub Release** (the source of truth) and the **website**
(sarke-nine.vercel.app, which downloads from Supabase Storage). Both get the same
`SarkeSetup-<version>.exe`.

## 1. Version

Bump both lines in [resources/version.h](../resources/version.h): every installer that goes out
has a new version.

```c
#define SARKE_VERSION_COMMA 1, 2, 0, 0
#define SARKE_VERSION_STRING "1.2.0"
```

Add a section for it to `CHANGELOG.md`. If the protocol changed (`kProtocolVersion` in
[protocol.h](../common/include/kvm/protocol.h)), say so at the top: both PCs must update.

## 2. Build the installer

```powershell
powershell -ExecutionPolicy Bypass -File tools\make_installer.ps1
```

It builds the release, runs the quick tests, fetches the pinned Virtual Display Driver
(`tools\fetch_vdd.ps1`) and writes `dist\SarkeSetup-<version>.exe`. It needs the build environment
and Inno Setup 6.

## 3. Test it

- **Fresh install** in Windows Sandbox or on a spare PC:
  - install it and start Sarke;
  - check the firewall rules with `netsh advfirewall firewall show rule name=Sarke`;
  - check the driver: Device Manager → Display adapters → Virtual Display Driver;
  - uninstall, and check that the rules are gone.
- **Upgrade** over the previous release.
- **Two PCs:** the [regression checklist](ROADMAP.md#regression-checklist).

## 4. Tag and publish the GitHub Release

```powershell
git tag -a v1.2.0 -m "Sarke 1.2.0"     # on the commit the installer was built from
git push origin v1.2.0
(Get-FileHash dist\SarkeSetup-1.2.0.0.exe -Algorithm SHA256).Hash
```

Then on GitHub:
1. Go to **Releases** → **Draft a new release** and pick the tag. The title is `Sarke 1.2.0`.
2. Paste the CHANGELOG section and the SHA-256 into the notes.
3. Attach `SarkeSetup-1.2.0.0.exe` and publish.

The README links to the latest release.

## 5. Publish to the website

```powershell
$env:SUPABASE_URL = 'https://<project>.supabase.co'
$env:SUPABASE_SERVICE_ROLE_KEY = '<service_role key>'   # Supabase dashboard -> Project Settings -> API
powershell -ExecutionPolicy Bypass -File tools\publish_release.ps1
```

This uploads the newest installer as `SarkeSetup.exe` (the website's download button), as
`SarkeSetup-<version>.exe`, and as `latest.json` (version, links and SHA-256). The key comes from the
environment and never goes into the repo.

## 6. Check

Download the installer from the GitHub Release and from the website. Both must have the SHA-256
from step 4.
