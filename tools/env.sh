# source tools/env.sh  -- puts the WinLibs GCC toolchain on PATH (Git Bash / MSYS2)
WINLIBS="$LOCALAPPDATA/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
if [ -d "$WINLIBS" ]; then
  export PATH="$(cygpath -u "$WINLIBS" 2>/dev/null || echo "$WINLIBS"):$PATH"
fi
alias make=mingw32-make 2>/dev/null
