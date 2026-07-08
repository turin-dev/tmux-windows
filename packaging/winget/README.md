# winget manifests

Manifests for publishing tmuxw to the Windows Package Manager (`winget`).
`InstallerType: portable` — winget places `tmuxw.exe` on the PATH; there is no
MSI and no VC++ redistributable (the CRT is statically linked).

## Regenerating for a new version

1. Build and package the release (`scripts\package.bat <version>`).
2. Create the GitHub release with `tmuxw-<version>-win-x64.exe` attached.
3. Update `PackageVersion`, `InstallerUrl`, and `InstallerSha256` in these files.
   The SHA-256 is printed by `package.bat` (or: `Get-FileHash <exe> -Algorithm SHA256`).

## Validate locally

```powershell
winget validate --manifest packaging\winget
winget install --manifest packaging\winget      # local test install
```

## Publish to the public winget repository

Submit a PR to [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)
under `manifests/t/Turin/tmuxw/0.1.0/`, or use
[`wingetcreate`](https://github.com/microsoft/winget-create):

```powershell
wingetcreate submit --token <gh-token> packaging\winget
```

Until it is accepted upstream, users can install directly from a manifest:

```powershell
winget install --manifest packaging\winget
```
