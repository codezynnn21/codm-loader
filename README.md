# codm skin loader

Two pieces in one repo:

- `codm-skin/` — frida kit: recon + client-side skin id patch for CODM.
- root gradle project — root loader apk that stages `frida-server` + `codm.js`
  from its own assets, starts the server as root, and launches the game.

CI (`.github/workflows/build.yml`) downloads the matching `frida-server`
arm64 binary, bakes it into the apk assets, and uploads `app-debug.apk`.

## build

local:

```powershell
gradle :app:assembleDebug
```

github:

1. push the repo, `main` branch (or run the workflow manually).
2. actions -> `build-loader` -> artifact `codm-loader-debug`.
3. install `app-debug.apk` on the rooted device/emulator.

the workflow input `frida_version` must match your host `frida --version`
exactly; the device server and host tools refuse mismatched versions.

## use

1. open `codm loader`, grant root.
2. `install` stages the server + script into app storage.
3. `start` runs `frida-server` as root.
4. `launch` opens CODM.
5. on the pc, run the frida session against the device:
   ```powershell
   frida -U -f com.activision.callofduty.shooter -l codm.js
   ```
   (the kit's `push.ps1` does the same thing with adb, if you prefer that route)

recon and patch rules live in `codm-skin/README.md`.

## notes

- the apk does not inject anything by itself — `frida-server` does the loading,
  the apk just carries it and starts it as root.
- `codm-skin/codm.js` reads its config from `/data/local/tmp/codm_config.json`
  when started via `push.ps1`; when started through this apk, point the host
  frida session at the script and edit rules in `codm-skin/config.json` before
  running `push.ps1 patch`.
- release builds are signed with the debug key so they install without setup;
  swap in a real keystore before distributing.
