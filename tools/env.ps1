# . .\tools\env.ps1  -- puts the WinLibs GCC toolchain on PATH (PowerShell)
$winlibs = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
if (Test-Path $winlibs) { $env:PATH = "$winlibs;$env:PATH" }
Set-Alias make mingw32-make
