# Prjct zxore loader — native, no frida

Root loader for Call of Duty: Mobile (Garena + global + CN + KR + VN).
The apk carries a ptrace injector and an il2cpp hook library; the injector
dlopens the hook lib into the running game process, the hook lib resolves
il2cpp and applies the rules from `/data/local/tmp/codm_config.json`.

No frida, no pc-side tooling at runtime. Root required. UI is a port of the
KOYRI loader reference: splash gate -> menu, framework-only, no androidx,
recoloured to the Prjct zxore black/gray theme.

## tree

```
codm/
├── .github/workflows/build.yml     CI: ndk + cmake + apk artifact
├── build.gradle                    AGP 8.5.2 / kotlin 1.9.24
├── settings.gradle
├── gradle.properties
├── README.md                       this file
├── app/
│   ├── build.gradle                externalNativeBuild, asset ordering, legacy jni packaging
│   └── src/main/
│       ├── AndroidManifest.xml     theme @style/Theme.Koyri, package queries
│       ├── assets/
│       │   └── codm_config.json    staged to /data/local/tmp on every inject
│       ├── res/
│       │   ├── drawable-nodpi/ic_logo.png   512px mark, drawn in a circle clip
│       │   ├── mipmap-*/                    launcher pngs + adaptive foreground
│       │   ├── mipmap-anydpi-v26/           adaptive icon xml (background + fg)
│       │   └── values/                      styles.xml, strings.xml, colors.xml
│       ├── cpp/
│       │   ├── CMakeLists.txt      injector exe + codmhook .so (dobby)
│       │   ├── injector/main.c     ptrace: mmap -> dlopen -> detach
│       │   └── hook/hook.cpp       il2cpp resolve, method/patch/nop, hide_self
│       └── java/gg/ollie/codmloader/
│           ├── KoyriUi.kt          palette, type, dp, card/pill, canvas text
│           ├── SplashView.kt       splash gate: ticks, sweep ring, bar, steps
│           └── MainActivity.kt     menu: target / payload / runtime / log + inject
├── art/
│   └── source.jpg                  icon/logo source frame (690x1280)
├── tools/
│   └── make_icons.ps1              regenerates every png from art/source.jpg
└── codm-skin/
    ├── config.json                 mode, filter, rules
    ├── codm.js                     frida fallback route
    ├── push.ps1                    adb helper for the frida route
    ├── dump_to_rules.py            dump.cs -> rules
    └── README.md                   config + recon manual
```

## ui

Ported from the KOYRI reference (`KOYRI LOADER UI.zip`), same metrics and
layout, rebranded `PRJCT ZXORE` in black and gray:

- splash: gradient backdrop, grid, 64-tick progress ring, sweep arc, logo
  disc, shimmer brand, determinate bar with live percentage, five step rows
  (ROOT ACCESS / INJECTOR STAGE / PAYLOAD HASH / TARGET SCAN / ENVIRONMENT).
- menu: `Theme.Koyri` Material, solid `BG_BOTTOM`, header mark + brand +
  status badge, hairline-separated cards, ACCENT section titles, status
  strip, ACCENT->ACCENT_DIM CTA, three ghost buttons, mono watermark.

Palette: BG_TOP `#111111`, BG_BOTTOM `#000000`, CARD `#141414`, CARD_EDGE
`#262626`, HAIRLINE `#2E2E2E`, TEXT `#F2F2F2`, TEXT_DIM `#9A9A9A`,
TEXT_FAINT `#5C5C5C`, ACCENT `#EDEDED`, ACCENT_DIM `#8A8A8A`, OK `#F5F5F5`,
WARN `#B0B0B0`, BAD `#707070`.

No hue anywhere: every gradient, glow, tick, dot and status colour is a
white/gray ramp. The logo is baked to gray by `tools/make_icons.ps1` and then
filtered again at draw time through `KoyriUi.GRAY` — a saturation-0
`ColorMatrixColorFilter` — so a colour source frame needs no code change.

Brand strings live in `res/values/strings.xml` (`app_name`, `brand`,
`tagline`), the splash word in `SplashView.WORD`, the watermark in
`MainActivity.watermark()`. Internals stay CODM: package list,
`libcodmhook.so`, `/data/local/tmp/codm_config.json`, injector lane.

No androidx dependency: every screen is framework `View` code, so the apk
carries no support library.

### icon + logo

Both come from one source frame, cropped to a 430px square at `(200, 140)` —
head, blindfold, maid headband and the hands on the hilt — then desaturated
through a saturation-0 colour matrix so it matches the theme:

| asset | size | use |
|---|---|---|
| `drawable-nodpi/ic_logo.png` | 512 | splash disc + menu mark, circle-clipped |
| `mipmap-*/ic_launcher.png` | 48 / 72 / 96 / 144 / 192 | legacy launcher icon |
| `mipmap-*/ic_launcher_round.png` | same | legacy round icon, ellipse mask |
| `mipmap-*/ic_launcher_foreground.png` | 108 / 162 / 216 / 324 / 432 | adaptive foreground, full bleed |

`mipmap-anydpi-v26/ic_launcher{,_round}.xml` wraps the foreground over
`@color/ic_launcher_background` (`#FF000000`), so API 26+ gets a proper
adaptive icon and older devices fall back to the PNG pair.

The in-app mark is additionally filtered at draw time through
`KoyriUi.GRAY`, so swapping the source frame for a colour image needs no
code change.

regenerate after replacing `art/source.jpg`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/make_icons.ps1
```

the crop window lives at the top of that script (`$cx = 200`, `$cy = 140`,
`$cs = 430`) — move it to reframe the face on a different source frame.

## build

local:

```powershell
gradle :app:assembleDebug
```

github: push to `main`, actions -> `build-loader` -> artifact `codm-loader-debug`.
CI installs `ndk;27.0.12077973` and `cmake;3.22.1`, then gradle runs the cmake
build. dobby is fetched by cmake at configure time.

## run

1. install `app-debug.apk`, open it, grant root.
2. the splash runs the five checks, then the menu opens.
3. `LAUNCH` — starts the selected package (default `com.garena.game.codm` when
   installed). Tap the PACKAGE row to cycle the installed builds.
4. wait for the game to reach its login/menu screen.
5. `INJECT` — stages the injector + config, runs ptrace injection.
6. `adb logcat -s codmhook` shows hook results.

`STAGE` does the staging without injecting. `STOP` force-stops the target.

config lives at `/data/local/tmp/codm_config.json` and is pushed from
`app/src/main/assets/codm_config.json` on every stage/inject.

## rules

`app/src/main/assets/codm_config.json` (and `codm-skin/config.json`, same schema):

| kind | meaning |
|---|---|
| `method` | resolve `class::method` (params) and force its int return to `value` |
| `patch` | write `value` (u64) at `libil2cpp.so` base + `offset` |
| `nop` | write `value` NOP instructions at `libil2cpp.so` base + `offset` |

`mode: "recon"` writes `/data/local/tmp/codm_recon.json` with every class whose
name matches `filter`, its methods (name/params/returns) and fields (offset).
Run recon once, pick your names, switch to `patch`.

Offset rules resolve the game module through `dl_iterate_phdr` — the base is
`libil2cpp.so`'s load bias, not the hook lib's. A rule with an empty `offset`
is skipped with a log line instead of writing to the ELF header.

### ban-lock / penalty bypass

The ban state itself is server-side — no client patch lifts it. What the
`patch`/`nop` kinds give you is the client-side skip: point them at the
client functions that raise the lock UI / report state / gate the matchmaking
button, using RVAs from your own dump. Example shape:

```json
{ "kind": "nop",   "offset": "0x1A2B3C", "value": 4 },
{ "kind": "patch", "offset": "0x1A2B50", "value": 1 }
```

Fill those offsets from `codm_recon.json` (method names containing `ban`,
`lock`, `penalty`, `restrict`) or from `dump.cs` via `dump_to_rules.py`.

## limits

- arm64 only; the injector is arch-specific.
- the hook lib is dlopened from `nativeLibraryDir`, so the apk keeps
  `extractNativeLibs=true` / `useLegacyPackaging true`.
- injection is visible to root-detection and ptrace checks; `hide_self()` unlinks
  the hook lib from the linker's `link_map`, nothing more.
- `DobbyHook` rule signatures are fixed to il2cpp instance getters
  `(this, MethodInfo*)` with `params: 0` returning int. Offset rules have no
  signature constraint.
- use alt accounts. bans are account-level and server-side.
