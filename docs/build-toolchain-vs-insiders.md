# Buildear MuMain cuando "no hay toolchain" (VS Insiders + feed NuGet privado)

Guía para desbloquear el build del cliente MuMain en una máquina donde, a primera
vista, parece que faltan herramientas. Dos problemas independientes que aparecen
juntos en este entorno.

---

## Problema 1 — cmake / ninja / compilador C++ "command not found"

**Síntoma:** `cmake`, `ninja`, `cl`, `clang`, `gcc` dan *command not found*; `vswhere`
devuelve vacío → parece que no hay toolchain C++.

**Causa raíz:** la máquina tiene **Visual Studio 2026 Insiders** (prerelease). Dos trampas:
1. `vswhere` **oculta instalaciones prerelease** salvo que se pase `-prerelease`.
2. El toolchain (cmake/ninja/MSVC) **no está en el PATH** — vive *bundled* dentro de VS;
   hay que cargarlo a mano vía `vcvarsall`.

### Detección
```bash
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
"$VSWHERE" -prerelease -products '*' \
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
  -property installationPath
# => C:\Program Files\Microsoft Visual Studio\18\Insiders
```

### Rutas bundled dentro de ese install
```
vcvarsall: <VS>\VC\Auxiliary\Build\vcvarsall.bat
cmake:     <VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
ninja:     <VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
```
Verificado en este entorno: `cmake 4.3.1-msvc1`, `cl.exe 14.51 (Hostx86\x86)`.

### Wrapper `.bat` (carga MSVC x86 + cmake/ninja en PATH, luego ejecuta los args)
Guardar como `%TEMP%\mu_build.bat`:
```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
%*
```
> `vcvarsall x86` porque el target es **32-bit** (`toolchain-x86.cmake` fuerza
> `CMAKE_SIZEOF_VOID_P 4`). Para x64 usar `vcvarsall x64` + preset `windows-x64`.

---

## Problema 2 — `dotnet restore` falla: feed NuGet privado inalcanzable

**Síntoma:** el `cmake configure` muere en
`src/CMakeLists.txt:374 dotnet restore failed (rc=1)` con muchos
`error NU1301 ... Host desconocido (nugets.trabajo.gob.ar:443)`.

**Causa raíz:** CMake compila la `ClientLibrary` (C#, Native AOT) y corre
`dotnet restore`. El config NuGet de usuario (`%APPDATA%\NuGet\NuGet.Config`) tiene
un **feed privado de intranet** (`nugets.trabajo.gob.ar` / origen `MTEySS`) que **no
resuelve fuera de esa red**. NuGet intenta todas las fuentes habilitadas y falla.
`nuget.org` está registrado y es alcanzable, pero la fuente caída aborta el restore.

> Nota de contexto: en el monorepo, el commit de "wiring" hace que la ClientLibrary
> consuma la librería de red local de OpenMU en vez de paquetes NuGet. Si trabajás
> sobre un branch ANTERIOR a ese wiring, la ClientLibrary todavía depende de los
> paquetes (`MUnique.OpenMU.PlugIns`, etc.) y necesita un feed que los tenga.

### Fix primario — quitar/deshabilitar la fuente caída del config global
```bash
dotnet nuget list source                 # ver el nombre de la fuente intranet
dotnet nuget remove source <NombreFuente>     # p.ej. MTEySS  (o: disable source)
# re-agregar luego si hace falta en la red de trabajo:
# dotnet nuget add source https://nugets.trabajo.gob.ar/nuget -n MTEySS
```
Tras quitarla, `dotnet nuget list source` debe dejar sólo fuentes alcanzables
(`nuget.org`, etc.). Todos los paquetes referenciados son públicos en `nuget.org`
(Microsoft.Extensions.*, Nito.AsyncEx.*, Microsoft.CodeAnalysis.*, MUnique.OpenMU.*,
ILCompiler de Native AOT).

### Alternativas (sin tocar el config global)
- `nuget.config` local en la raíz del repo con `<clear/>` + sólo `nuget.org`:
  ```xml
  <configuration>
    <packageSources>
      <clear />
      <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
    </packageSources>
  </configuration>
  ```
- Puntual: `dotnet restore <csproj> --ignore-failed-sources`.

---

## Secuencia completa de build

```bash
# 0) (una vez) submódulos
git submodule update --init                       # SDL, SDL_mixer, imgui

# 1) (una vez) nuget.config local con <clear/> + nuget.org   (Problema 2)

# 2) configure / build / test  — todo vía el wrapper (Problema 1)
cmd.exe //c "%TEMP%\mu_build.bat cmake --preset windows-x86 -DBUILD_TESTING=ON"
cmd.exe //c "%TEMP%\mu_build.bat cmake --build --preset windows-x86-debug"
ctest --test-dir out/build/windows-x86 -C Debug --output-on-failure
```

- Build con editor in-game: presets `windows-x86-mueditor` / `windows-x86-mueditor-debug`.
- Ejecutar: `out/build/windows-x86/src/Debug/Main.exe`
  (params: `Main.exe connect /u<ip> /p<port>`, default localhost:44406).
- Requiere `.NET SDK 10` (para la ClientLibrary AOT). `dotnet` ya presente en este entorno.

## Checklist rápido para el otro agente
1. `vswhere -prerelease` → encontrar VS Insiders.
2. Crear `%TEMP%\mu_build.bat` (wrapper vcvars + cmake/ninja bundled).
3. `git submodule update --init`.
4. Crear `nuget.config` con `<clear/>` + nuget.org en la raíz del repo.
5. configure → build → ctest, siempre por el wrapper.
