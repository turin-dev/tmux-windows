# Releasing tmuxw

tmuxw ships as a single portable `tmuxw.exe` (statically linked CRT — no VC++
redistributable required).

## Automated (recommended)

Push a version tag; the `release` GitHub Actions workflow builds, tests,
packages, and publishes a GitHub Release with the zip and exe attached.

```
git tag v0.1.0
git push origin v0.1.0
```

## Manual

```
scripts\package.bat 0.1.0
```

Produces in `dist\`:

- `tmuxw-0.1.0-win-x64.zip` — portable bundle (exe + docs)
- `tmuxw-0.1.0-win-x64.exe` — standalone binary
- SHA-256 hashes (printed to the console)

Create the release and upload the assets:

```
gh release create v0.1.0 dist\tmuxw-0.1.0-win-x64.zip dist\tmuxw-0.1.0-win-x64.exe ^
  --title "tmuxw 0.1.0" --generate-notes
```

## winget

After the release exists, update `packaging\winget\*.yaml` with the new version,
`InstallerUrl`, and `InstallerSha256`, then validate and submit. See
[`packaging/winget/README.md`](packaging/winget/README.md).
