# codm-skin

Client-side skin id patcher for Call of Duty: Mobile (Unity il2cpp, arm64).
Frida loads `codm.js` into the game process; recon finds the skin getters for
your build, patch forces their return value. Server inventory is untouched —
skins are visible to you only.

## tree

| file | role |
|---|---|
| `codm.js` | frida script — recon mode + patch mode |
| `config.json` | mode, scan filter, patch rules |
| `push.ps1` | adb helper — push, frida-server, spawn, pull |
| `dump_to_rules.py` | optional: Il2CppDumper `dump.cs` -> recon json / rule templates |
| `README.md` | this file |
| `frida-server` | you drop the arm64 binary here (setup pushes it) |

## prerequisites

- python 3 + `pip install frida-tools`
- `adb` on PATH (platform-tools)
- rooted device or emulator (ldplayer / mumu, arm64 image)
- `frida-server` matching the host frida version exactly:
  https://github.com/frida/frida/releases -> `frida-server-<ver>-android-arm64.xz`
  unzip it, rename to `frida-server`, place next to this README

## quick start

1. `frida --version` on the pc. download that exact server version.
2. put `frida-server` in this folder.
3. connect device, then:
   ```powershell
   .\push.ps1 setup      # push server + script + config, start server
   .\push.ps1 recon      # spawn game, scan, pull codm_recon.json
   ```
4. open `codm_recon.json` — every class whose name matched the filter, with
   `methods[]` (`name`, `params`, `returns`, `address`) and `fields[]` (`name`, `offset`).
5. pick the getter that returns the equipped skin id, write a rule into
   `config.json`, set `"mode": "patch"`:
   ```json
   {
     "mode": "patch",
     "rules": [
       { "class": "Namespace.PlayerWeapon", "method": "get_CurrentSkinId", "params": 0, "value": 1001 }
     ]
   }
   ```
6. ```powershell
   .\push.ps1 patch      # spawn with rules live
   ```

## optional: dump.cs route

No dump is required — recon runs against the live process. If you already have
an `Il2CppDumper` dump, it gives you names and RVAs without spawning:

```bash
Il2CppDumper.exe libil2cpp.so global-metadata.dat out
python dump_to_rules.py out/dump.cs -f "skin|camo|weapon" -o codm_dump.json -r config.json
```

`rva` values from the dump are relative to `libil2cpp.so`, so they drop into a
rule as `"offset": "0x1A2B3C"` with no class lookup.

## config reference

| key | meaning |
|---|---|
| `mode` | `recon` writes the report, `patch` installs hooks |
| `filter` | regex over `namespace.class` for the scan |
| `output` | device path the recon report lands at |
| `rules[].class` | full class name from the report |
| `rules[].method` | method name from the report |
| `rules[].params` | param count, disambiguates overloads |
| `rules[].offset` | alternative to class/method: libil2cpp RVA in hex, e.g. `"0x8F2A10"` |
| `rules[].value` | number forced into the return register |

Every rule is independent; a failed rule logs and the rest stay live.

## package names

| build | package |
|---|---|
| global | `com.activision.callofduty.shooter` |
| china | `com.tencent.tmgp.cod` |
| garena | `com.garena.game.codm` |
| korea | `com.tencent.tmgp.kr.codm` |
| vietnam | `com.vng.codmvn` |

## troubleshooting

| symptom | cause |
|---|---|
| `il2cpp init failed` | script ran before the engine loaded — spawn with `-f`, not attach |
| recon report empty | filter too narrow, or class enumeration exports missing on the build — widen filter to `.*` |
| rule: class not found | wrong namespace — copy the exact `klass` string from the report |
| hooked but no visual change | getter is not the render path; hook the apply/equip setter or the id passed into the material swap |
| frida-server exits instantly | version mismatch or missing root |
| game crashes on spawn | server arch mismatch (arm64 vs x86) — use an arm64 image |

## limits

Client-side only. The game server rejects inventory changes; this forces the
visual id the client already knows how to render. Forcing an id the build does
not ship renders the default skin.

frida-server is trivially visible to ACE — emulator and alt accounts only.
