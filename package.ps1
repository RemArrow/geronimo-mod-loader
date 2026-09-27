# Writes the release zips to build\release\ from an existing build (run build.bat first,
# or use `build.bat package`).
#   GML-<ver>.zip      extract into <game>\Geronimo\Binaries\Win64\
#   GML-SDK-<ver>.zip  headers + example plugin + gmlcheck, for plugin authors
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'
$ver = [regex]::Match((Get-Content (Join-Path $root 'src\internal.h') -Raw), 'GML_VERSION_STRING "([^"]+)"').Groups[1].Value
if (-not $ver) { throw 'GML_VERSION_STRING not found in src\internal.h' }
$rel = Join-Path $out 'release'
Remove-Item -Recurse -Force $rel -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $rel | Out-Null

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

# Zip entries with '/' separators (Compress-Archive in Windows PowerShell 5.1 writes '\', which
# other unzip tools treat as part of the file name). Values: a file path, or literal text.
function Stage($name, $files) {
    $zip = Join-Path $rel "$name.zip"
    $archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
    try {
        foreach ($f in $files.GetEnumerator()) {
            $entry = $f.Key -replace '\\', '/'
            if ($f.Value -match '^[A-Za-z]:\\' ) {
                if (-not (Test-Path $f.Value)) { throw "missing: $($f.Value)" }
                [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.Value, $entry, 'Optimal') | Out-Null
            } else {
                $w = New-Object IO.StreamWriter($archive.CreateEntry($entry, 'Optimal').Open())
                $w.Write(($f.Value -replace "`r?`n", "`r`n") + "`r`n")
                $w.Close()
            }
        }
    } finally { $archive.Dispose() }
    Get-Item $zip
}

$install = @"
Geronimo Mod Loader $ver
https://github.com/RemArrow/geronimo-mod-loader

Install: extract this zip into
  <Steam library>\steamapps\common\GERONIMO\Geronimo\Binaries\Win64\
(the folder containing Geronimo-Win64-Shipping.exe). Close the game first.

Then put plugins in GML\plugins\ (any subfolder), patchers in GML\patchers\.
Settings appear in GML\config\ after the first launch; the log is GML\LogOutput.log.

Disable: set  enabled = false  in doorstop_config.ini.
Uninstall: delete version.dll, doorstop_config.ini, GML_INSTALL.txt, the GML folder and
..\..\Content\Paks\~GML.
"@

$game = Join-Path $out 'game'
Stage "GML-$ver" ([ordered]@{
    'version.dll'              = Join-Path $game 'version.dll'
    'doorstop_config.ini'      = Join-Path $game 'doorstop_config.ini'
    'GML\core\GML.dll'         = Join-Path $game 'GML\core\GML.dll'
    'GML\plugins\README.txt'   = "Plugins go here (any subfolder depth). See https://github.com/RemArrow/geronimo-mod-loader"
    'GML\patchers\README.txt'  = "Patchers go here: DLLs exporting GML_Patch, run at process entry before the engine starts."
    'GML_INSTALL.txt'          = $install
})

Stage "GML-SDK-$ver" ([ordered]@{
    'include\GML\GML.h'             = Join-Path $root 'include\GML\GML.h'
    'include\GML\GML.hpp'           = Join-Path $root 'include\GML\GML.hpp'
    'examples\HelloGML\HelloGML.cpp' = Join-Path $root 'examples\HelloGML\HelloGML.cpp'
    'tools\gmlcheck.exe'            = Join-Path $out 'gmlcheck.exe'
    'README.md'                     = Join-Path $root 'README.md'
})
