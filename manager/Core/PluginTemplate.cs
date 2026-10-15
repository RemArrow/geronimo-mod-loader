// "New plugin project": a ready-to-build plugin folder with the SDK headers this manager was
// built with, a build.bat that finds Visual Studio by itself, and an install step.
using System;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;

namespace GMLManager.Core
{
    public static class PluginTemplate
    {
        public static bool ValidName(string n) => Regex.IsMatch(n ?? "", @"^[A-Za-z][A-Za-z0-9_]{0,63}$");
        public static bool ValidGuid(string g) => Regex.IsMatch(g ?? "", @"^[a-z0-9_]+(\.[a-z0-9_]+){2,}$");

        public static string Create(string parent, string name, string guid, string author, string win64)
        {
            var dir = Path.Combine(parent, name);
            if (Directory.Exists(dir) && Directory.GetFileSystemEntries(dir).Length > 0)
                throw new IOException($"{dir} already exists and isn't empty.");
            Directory.CreateDirectory(Path.Combine(dir, "src"));
            Directory.CreateDirectory(Path.Combine(dir, "sdk", "include", "GML"));
            foreach (var h in new[] { "GML.h", "GML.hpp" })
                using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("sdk/" + h))
                using (var f = File.Create(Path.Combine(dir, "sdk", "include", "GML", h)))
                    s.CopyTo(f);

            var utf8 = new UTF8Encoding(false);
            File.WriteAllText(Path.Combine(dir, "src", name + ".cpp"), $@"// {name} - a GML plugin for Geronimo.
// Build: build.bat   Install into the game: build.bat install   (the game must be closed)
// Log: <game>\Geronimo\Binaries\Win64\GML\LogOutput.log   Settings: GML\config\{guid}.cfg
#include <GML/GML.hpp>

using namespace gml;

GML_PLUGIN(""{guid}"", ""{name}"", ""0.1.0"");

static ConfigEntry<bool> s_enabled;

GML_AWAKE() {{
    s_enabled = Config.Bind(""General"", ""Enabled"", true, ""Switch {name} on or off."");
    if (!s_enabled.Value()) return 0;
    Log(""{name} loaded"");

    // Runs at every level start, the main menu included.
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gameMode) {{
        Log(""level started: {{}}"", Object((GUObject*)gameMode).FullName());
    }});

    // Runs after the gun station builds its list of guns.
    HookAfter(""BP_GameInstance_C:GetItems"", [](Object gi, GUFunction*, void*) {{
        Log(""spawner list built on {{}}"", gi.Name());
    }});
    return 0;   // non-zero = failed to start
}}
".Replace("\r\n", "\n").Replace("\n", "\r\n"), utf8);

            File.WriteAllText(Path.Combine(dir, "build.bat"), $@"@echo off
rem Builds {name}.dll.   build.bat          -> build\{name}.dll
rem                      build.bat install  -> also copies it into the game (close the game first)
rem Needs Visual Studio 2022 (or Build Tools) with the ""Desktop development with C++"" workload.
setlocal
set ROOT=%~dp0
set NAME={name}
set GAME={win64}
if defined GML_GAME_DIR set GAME=%GML_GAME_DIR%

rem (labels, not blocks: the game path's ""(x86)"" would end a parenthesised block)
if defined VCToolsInstallDir goto build
set ""VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe""
for /f ""usebackq delims="" %%I in (`""%VSWHERE%"" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set ""VS=%%I""
if not defined VS (echo Visual Studio with C++ tools not found.& exit /b 1)
call ""%VS%\VC\Auxiliary\Build\vcvars64.bat"" >nul 2>nul || exit /b 1

:build
if not exist ""%ROOT%build\obj"" mkdir ""%ROOT%build\obj""
pushd ""%ROOT%""
cl /nologo /std:c++20 /O2 /MT /EHsc /W3 /Zi /DUNICODE /D_UNICODE /I""%ROOT%sdk\include"" /LD /Fo""%ROOT%build\obj\\"" /Fd""%ROOT%build\obj\\"" ""%ROOT%src\*.cpp"" /Fe""%ROOT%build\%NAME%.dll"" /link /DEBUG /INCREMENTAL:NO
set ERR=%ERRORLEVEL%
popd
if not ""%ERR%""==""0"" exit /b 1
del /q ""%ROOT%build\%NAME%.exp"" ""%ROOT%build\%NAME%.lib"" 2>nul
if /i not ""%1""==""install"" goto done

tasklist /fi ""imagename eq Geronimo-Win64-Shipping.exe"" | find /i ""Geronimo"" >nul && echo Close Geronimo first.&& exit /b 1
if not exist ""%GAME%\GML\plugins\%NAME%"" mkdir ""%GAME%\GML\plugins\%NAME%""
copy /y ""%ROOT%build\%NAME%.dll"" ""%GAME%\GML\plugins\%NAME%\"" >nul || exit /b 1
copy /y ""%ROOT%build\%NAME%.pdb"" ""%GAME%\GML\plugins\%NAME%\"" >nul
echo installed to ""%GAME%\GML\plugins\%NAME%""

:done
echo done
".Replace("\r\n", "\n").Replace("\n", "\r\n"), utf8);

            File.WriteAllText(Path.Combine(dir, "README.md"), $@"# {name}

A [GML](https://github.com/RemArrow/geronimo-mod-loader) plugin for Geronimo{(string.IsNullOrWhiteSpace(author) ? "" : " by " + author)}.

## Build

```bat
build.bat            :: build\{name}.dll
build.bat install    :: and copy it into the game's GML\plugins\{name}\
```

Needs Visual Studio 2022 (or Build Tools) with *Desktop development with C++*. The GML SDK headers
are in `sdk\include` (GML {App.Version}). The API is documented in
[the GML README](https://github.com/RemArrow/geronimo-mod-loader#writing-a-plugin).

## Release

Zip it laid out like the game's `Binaries\Win64` folder, so players (and GML Manager) can extract
it there:

```
{name}-0.1.0.zip
  GML\plugins\{name}\{name}.dll
```

Attach the zip to a GitHub release. To list it in GML Manager's Browse tab, open a pull request
adding it to [catalog.json](https://github.com/RemArrow/geronimo-mod-loader/blob/main/catalog.json).
".Replace("\r\n", "\n").Replace("\n", "\r\n"), utf8);

            File.WriteAllText(Path.Combine(dir, ".gitignore"), "build/\r\n", utf8);
            return dir;
        }
    }
}
