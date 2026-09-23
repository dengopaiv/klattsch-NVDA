# Build and verify the C engine with every compiler available on this machine.
#
#   pwsh tools/build-matrix.ps1            build all, run the stage verifiers
#   pwsh tools/build-matrix.ps1 -Compare   also diff the compilers against each other
#
# The stage 6 exit test requires MSVC, clang-cl and gcc to produce identical
# samples. That only works if all of them have been runnable all along, so this
# runs from stage 1 rather than appearing at the end.
#
# Why four and not three: MSVC, clang-cl and WinLibs gcc are all UCRT on
# Windows, so agreement between them says little about libm -- they may be
# calling the same sin. WSL's Debian gcc is glibc, and that is the leg that
# actually tests the tolerance. It earns its place by disagreeing: see
# docs/13-stage1-dsp.md.
#
# gcc is deliberately not on PATH in this environment (MSVC is the default
# toolchain and gcc must not shadow it), so its directory is prepended only for
# the invocation that needs it.

[CmdletBinding()]
param(
  [switch]$Compare,
  [string]$VsRoot   = "C:\Program Files\Microsoft Visual Studio\18\Community",
  [string]$MinGWBin = "C:\GIT\environment\winlibs\mingw64\bin",
  [string]$WslDistro = "Debian"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$sections = @("lfsr", "softclip", "pulse", "biquad", "cache")
$results = @()

function Add-Result($name, $status, $detail) {
  $script:results += [pscustomobject]@{ Toolchain = $name; Status = $status; Detail = $detail }
}

$vcvars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
$clangCl = Join-Path $VsRoot "VC\Tools\Llvm\x64\bin\clang-cl.exe"

# --- MSVC -------------------------------------------------------------------

if (Test-Path $vcvars) {
  cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$repo`" && cmake -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release >nul && cmake --build build-msvc >nul"
  if ($LASTEXITCODE -eq 0) { Add-Result "MSVC (UCRT)" "built" "build-msvc" }
  else { Add-Result "MSVC (UCRT)" "BUILD FAILED" "" }
} else { Add-Result "MSVC (UCRT)" "skipped" "vcvars64.bat not found" }

# --- clang-cl ---------------------------------------------------------------

if ((Test-Path $vcvars) -and (Test-Path $clangCl)) {
  $c = $clangCl -replace '\\', '/'
  cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$repo`" && cmake -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=`"$c`" >nul && cmake --build build-clang >nul"
  if ($LASTEXITCODE -eq 0) { Add-Result "clang-cl (UCRT)" "built" "build-clang" }
  else { Add-Result "clang-cl (UCRT)" "BUILD FAILED" "" }
} else { Add-Result "clang-cl (UCRT)" "skipped" "clang-cl not found" }

# --- WinLibs gcc ------------------------------------------------------------

if (Test-Path (Join-Path $MinGWBin "gcc.exe")) {
  $saved = $env:Path
  try {
    $env:Path = "$MinGWBin;" + $env:Path
    Push-Location $repo
    & cmake -B build-gcc -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_C_COMPILER="$MinGWBin/gcc.exe" *> $null
    & cmake --build build-gcc *> $null
    Pop-Location
    if ($LASTEXITCODE -eq 0) { Add-Result "WinLibs gcc (UCRT)" "built" "build-gcc" }
    else { Add-Result "WinLibs gcc (UCRT)" "BUILD FAILED" "" }
  } finally { $env:Path = $saved }
} else { Add-Result "WinLibs gcc (UCRT)" "skipped" "$MinGWBin not found" }

# --- WSL gcc, the glibc leg -------------------------------------------------

$wslOk = $false
if (Get-Command wsl -ErrorAction SilentlyContinue) {
  $distros = (wsl -l -q) -replace "`0", "" | ForEach-Object { $_.Trim() } | Where-Object { $_ }
  if ($distros -contains $WslDistro) {
    $wslRepo = "/mnt/" + ($repo -replace '^([A-Za-z]):', { $_.Groups[1].Value.ToLower() } -replace '\\', '/')
    $sh = "cd '$wslRepo' && cmake -B build-wsl -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build-wsl >/dev/null && echo OK"
    $out = wsl -d $WslDistro -- bash -lc $sh 2>&1
    if ($out -match "OK") { Add-Result "WSL gcc (glibc)" "built" "build-wsl"; $wslOk = $true }
    else { Add-Result "WSL gcc (glibc)" "BUILD FAILED" ($out | Select-Object -Last 1) }
  } else { Add-Result "WSL gcc (glibc)" "skipped" "$WslDistro not installed" }
} else { Add-Result "WSL gcc (glibc)" "skipped" "wsl not present" }

$results | Format-Table -AutoSize | Out-String | Write-Host

# --- verify each build against the JavaScript -------------------------------

Write-Host "Stage verifiers`n"
foreach ($b in @("build-msvc", "build-clang", "build-gcc")) {
  $exe = Join-Path $repo "$b\kl_dsp_dump.exe"
  if (Test-Path $exe) {
    Write-Host "--- $b ---"
    & node (Join-Path $repo "tools\verify-stage1.mjs") $exe | Select-Object -Last 4 | Write-Host
  }
}

if ($wslOk) {
  Write-Host "--- build-wsl (dumped, verified from Windows: no node in WSL) ---"
  $dump = Join-Path $env:TEMP "klattsch-wsl-dump"
  New-Item -ItemType Directory -Force -Path $dump | Out-Null
  $wslDump = "/mnt/" + ($dump -replace '^([A-Za-z]):', { $_.Groups[1].Value.ToLower() } -replace '\\', '/')
  $wslRepo = "/mnt/" + ($repo -replace '^([A-Za-z]):', { $_.Groups[1].Value.ToLower() } -replace '\\', '/')
  # Through a script file rather than `bash -lc "..."`: a loop variable written
  # inline gets eaten somewhere between PowerShell, wsl.exe and bash, and the
  # dump tool is then called with no argument at all.
  $shFile = Join-Path $dump "dump.sh"
  $body = "set -e`ncd '$wslRepo'`nfor s in $($sections -join ' '); do`n  ./build-wsl/kl_dsp_dump `"`$s`" > '$wslDump'/`"`$s`".bin`ndone`n"
  [IO.File]::WriteAllText($shFile, ($body -replace "`r`n", "`n"))
  wsl -d $WslDistro -- bash "$wslDump/dump.sh" | Out-Null
  & node (Join-Path $repo "tools\verify-stage1.mjs") $dump | Select-Object -Last 4 | Write-Host
}

# --- do the compilers agree with each other? --------------------------------

if ($Compare) {
  Write-Host "`nCross-compiler comparison (SHA-256 of each section)`n"
  $rows = @()
  foreach ($s in $sections) {
    $row = [ordered]@{ Section = $s }
    foreach ($b in @("build-msvc", "build-clang", "build-gcc")) {
      $exe = Join-Path $repo "$b\kl_dsp_dump.exe"
      if (Test-Path $exe) {
        # Redirect through cmd rather than the pipeline: PowerShell would
        # decode the binary stream as text and the hash would be meaningless.
        $tmp = [IO.Path]::GetTempFileName()
        cmd /c "`"$exe`" $s > `"$tmp`""
        $row[$b.Replace("build-", "")] = (Get-FileHash $tmp -Algorithm SHA256).Hash.Substring(0, 12)
        Remove-Item $tmp -Force
      }
    }
    if ($wslOk) {
      $f = Join-Path $env:TEMP "klattsch-wsl-dump\$s.bin"
      if (Test-Path $f) { $row["wsl"] = (Get-FileHash $f -Algorithm SHA256).Hash.Substring(0, 12) }
    }
    $rows += [pscustomobject]$row
  }
  $rows | Format-Table -AutoSize | Out-String | Write-Host
  Write-Host "A section differing between the UCRT builds and wsl is expected for"
  Write-Host "anything calling sin or cos -- that is what the Tier 2 tolerance is for."
}
