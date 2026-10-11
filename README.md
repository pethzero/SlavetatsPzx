# SlavetatsPzx

SlaveTats as a C++/SKSE plugin, with an in-game menu built on [SKSE Menu Framework](https://github.com/QTR-Modding/SKSE-Menu-Framework-3).

Based on [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) by nopse0 (Apache-2.0). The tattoo logic, JContainers data and Papyrus API are SlaveTatsNG's; this fork adds the menu.

## What's different from SlaveTatsNG

* In-game menu (section **SlavetatsPzx** in SKSE Menu Framework) with Setup, Body, Head, Hand and Foot pages: pick section/pattern, ink, glow and glossy per overlay slot, then Save / Replace / Remove. Targets can be the player or the actor selected in the console.
* The menu uses the same SlaveTats data as the MCM (`.SlaveTats.applied`, `add_and_get_tattoo`, `synchronize_tattoos`), so both stay in sync.
* `blankTextureName` in the ini is read without its quotes.
* Output is `SlavetatsPzx.dll`. It **replaces** `SlaveTatsNG.dll`; don't install both, or the plugin loads twice.
* Config files stay in `Data/SKSE/Plugins/SlaveTatsNG/` (`SlaveTatsNG.ini`, `skee_addresses.json`).

## User requirements

* [Address Library for SKSE](https://www.nexusmods.com/skyrimspecialedition/mods/32444) (SE/AE) or [VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101) (VR)
* JContainers
* RaceMenu (SKEE)
* SKSE Menu Framework (only needed for the in-game menu)

## Building

Requirements:

* Visual Studio 2022 or newer with "Desktop development with C++"
* [vcpkg](https://github.com/microsoft/vcpkg), with the `VCPKG_ROOT` environment variable set (defaults to `C:\vcpkg`)

CommonLibSSE-NG and the other dependencies come from the vcpkg manifest (`vcpkg.json`, `vcpkg-configuration.json`).

```
build.bat                                      # release -> build\release-msvc\SlavetatsPzx.dll
build.bat debug                                # debug   -> build\debug-msvc
build.bat release "D:\MO2\mods\SlavetatsPzx"   # also copy DLL + config into a mod folder
```

`build.ps1` takes the same options (`-Preset`, `-Clean`, `-OutputDir`).

## Links

* [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) / [LoversLab page](https://www.loverslab.com/files/file/35989-slavetatsng/)
* [VMTHook](https://github.com/gfreivasc/VMTHook)

## License

[Apache-2.0](LICENSE), same as SlaveTatsNG.
