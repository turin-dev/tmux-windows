# Releasing tmuxw

tmuxw ships as a single portable `tmuxw.exe` (statically linked CRT — no VC++
redistributable required), plus a GUI **setup wizard** built with Inno Setup.

## Automated (recommended)

Push a version tag; the `release` GitHub Actions workflow builds, tests,
packages, builds the install wizard, and publishes a GitHub Release with the
zip, exe, and setup wizard attached. (Inno Setup ships on the `windows-latest`
runner, so no extra setup is needed.)

```
git tag v0.1.0
git push origin v0.1.0
```

## Manual

```
scripts\package.bat 0.1.0     REM portable zip + exe
scripts\installer.bat 0.1.0   REM GUI setup wizard (requires Inno Setup 6)
```

Produces in `dist\`:

- `tmuxw-0.1.0-win-x64.zip` — portable bundle (exe + docs)
- `tmuxw-0.1.0-win-x64.exe` — standalone binary
- `tmuxw-0.1.0-setup.exe` — custom install wizard
- SHA-256 hashes (printed to the console)

The wizard is defined in [`installer/tmuxw.iss`](installer/tmuxw.iss). Get Inno
Setup 6 from <https://jrsoftware.org/isdl.php> (or `winget install
JRSoftware.InnoSetup`).

Create the release and upload the assets:

```
gh release create v0.1.0 dist\tmuxw-0.1.0-win-x64.zip dist\tmuxw-0.1.0-win-x64.exe ^
  dist\tmuxw-0.1.0-setup.exe --title "tmuxw 0.1.0" --generate-notes
```

## winget

After the release exists, update `packaging\winget\*.yaml` with the new version,
`InstallerUrl`, and `InstallerSha256`, then validate and submit. See
[`packaging/WINGET.md`](packaging/WINGET.md).
