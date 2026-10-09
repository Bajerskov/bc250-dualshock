# Compiles and runs the host unit tests for led_protocol.h.
# Requires Visual Studio 2022 Community (cl.exe). Exits non-zero on failure.
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$root = Split-Path -Parent $PSScriptRoot
$out  = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $out | Out-Null

$src = Join-Path $PSScriptRoot "test_led_protocol.cpp"
$exe = Join-Path $out "test_led_protocol.exe"

# Compile from inside the build directory so object files land there. Do NOT
# use /Fo:"$out\" instead -- the trailing backslash escapes the closing quote
# in cmd and the compile dies with "C1083: Cannot open compiler generated
# file: '': Invalid argument".
#
# /wd4127 silences C4127 "conditional expression is constant", which /W4 emits
# for every assertion on a compile-time constant, e.g. CHECK(LED_MAX_COUNT == 17).
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$out`" && cl /nologo /EHsc /W4 /wd4127 /std:c++17 /I`"$root`" `"$src`" /Fe:test_led_protocol.exe"
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

& $exe
exit $LASTEXITCODE
