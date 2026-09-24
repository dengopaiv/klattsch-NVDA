# Build and verify the C engine with every compiler available on this machine.
#
#   pwsh tools/build-matrix.ps1            build all, run the stage verifiers
#   pwsh tools/build-matrix.ps1 -Compare   also diff the compilers against each other
#
# Covers stages 1 to 6. The -Compare table walks stage 1's sections only: they
# are the ones that can legitimately differ between libms, which is the whole
# reason the WSL leg is here.
#
# The stage 6 exit test requires MSVC, clang-cl and gcc to produce identical
# files. That only works if all of them have been runnable all along, so this
# runs from stage 1 rather than appearing at the end -- and stage 6 is the one
# stage where the CLI itself is run on every toolchain, not only its library.
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
# Per stage: the dump tool, its verifier, and the sections it emits.
#
# Stages 3, 4 and 5 were added in stage 5. Before that this script covered
# stages 1 and 2 only, and the later stages were built and verified per
# toolchain by hand -- which is worse, not better, and was written up as a
# weakness in ROADMAP.md rather than left implied.
$stages = @(
  @{ Stage = 1; Tool = "kl_dsp_dump";     Verify = "verify-stage1.mjs";
     Sections = @("lfsr", "softclip", "pulse", "biquad", "cache") },
  @{ Stage = 2; Tool = "kl_banks_dump";   Verify = "verify-stage2.mjs";
     Sections = @("banks", "probe") },
  @{ Stage = 3; Tool = "kl_synth_dump";   Verify = "verify-stage3.mjs" },
  @{ Stage = 4; Tool = "kl_token_dump";   Verify = "verify-stage4.mjs";
     Tool2 = "kl_norm_dump" },
  @{ Stage = 5; Tool = "kl_compile_dump"; Verify = "verify-stage5.mjs" },
  @{ Stage = 6; Tool = "kl_wav_dump";     Verify = "verify-stage6.mjs"; Cli = $true }
)
# The cross-compiler comparison walks stage 1's sections: they are the ones
# that can legitimately differ between libms. Stage 2 is pure table data.
$sections = $stages[0].Sections
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
  foreach ($st in $stages) {
    $exe = Join-Path $repo "$b\$($st.Tool).exe"
    if (Test-Path $exe) {
      Write-Host "--- $b, stage $($st.Stage) ---"
      # Stage 4 takes two tools: the tokenizer's dump and the normalizer's.
      $vargs = @($exe)
      if ($st.Tool2) { $vargs += (Join-Path $repo "$b\$($st.Tool2).exe") }
      # Stage 6 verifies the program as well as the library, so it is handed
      # the CLI this toolchain built rather than whichever one is on PATH.
      if ($st.Cli) {
        $cliExe = Join-Path $repo "$b\klattsch.exe"
        if (Test-Path $cliExe) { $vargs += @("--cli", $cliExe) }
      }
      & node (Join-Path $repo "tools\$($st.Verify)") @vargs | Select-Object -Last 3 | Write-Host
    }
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
  # The sample rates come from the goldens' own manifest rather than a list
  # repeated here, so a rate added to the corpus cannot silently stop being
  # covered on the one toolchain with a different libm.
  $manifest = Get-Content (Join-Path $repo "goldens\manifest.json") -Raw | ConvertFrom-Json
  $rates = $manifest.audioRates

  $lines = @("set -e", "cd '$wslRepo'")
  # Stage 1: one file per section.
  $lines += "for s in $($sections -join ' '); do"
  $lines += "  ./build-wsl/kl_dsp_dump `"`$s`" > '$wslDump'/`"`$s`".bin"
  $lines += "done"
  # Stage 3: one file per sample rate, named as verify-stage3 expects.
  $lines += "for r in $($rates -join ' '); do"
  $lines += "  ./build-wsl/kl_synth_dump goldens/schedules.bin `"`$r`" > '$wslDump'/`"`$r`".bin"
  $lines += "done"
  # Stages 4 and 5.
  $lines += "./build-wsl/kl_norm_dump --allcp > '$wslDump'/norm.bin"
  $lines += "./build-wsl/kl_token_dump goldens/cases-text.bin > '$wslDump'/tokens.bin"
  $lines += "./build-wsl/kl_token_dump --numbers goldens/numbers.bin > '$wslDump'/numbers.bin"
  $lines += "./build-wsl/kl_token_dump goldens/divergences.bin > '$wslDump'/divergences.bin"
  $lines += "./build-wsl/kl_compile_dump goldens/cases-compile.bin > '$wslDump'/compile.bin"
  # Stage 6: one file of whole WAVs per rate, the two sweeps, the encoder
  # goldens, and then the CLI itself run on each of the end-to-end texts.
  $lines += "for r in $($rates -join ' '); do"
  $lines += "  ./build-wsl/kl_wav_dump goldens/cases-compile.bin `"`$r`" > '$wslDump'/wav`"`$r`".bin"
  $lines += "done"
  $lines += "./build-wsl/kl_wav_dump --round-sweep > '$wslDump'/round-sweep.bin"
  $lines += "./build-wsl/kl_wav_dump --tofixed-sweep > '$wslDump'/tofixed-sweep.bin"
  $lines += "./build-wsl/kl_wav_dump --wav-goldens > '$wslDump'/wav-goldens.bin"
  # Each run happens in its own directory with the output called out.wav, so
  # that the line the program prints is the same one the JavaScript prints on
  # the Windows side -- the path is part of that line.
  $lines += "i=0"
  $lines += "while IFS= read -r t; do"
  $lines += "  mkdir -p '$wslDump'/run`$i"
  $lines += "  (cd '$wslDump'/run`$i && '$wslRepo'/build-wsl/klattsch `"`$t`" out.wav 2> '$wslDump'/cli-`$i.err)"
  $lines += "  mv '$wslDump'/run`$i/out.wav '$wslDump'/cli-`$i.wav"
  $lines += "  i=`$((i+1))"
  $lines += "done < '$wslDump'/cli-texts.txt"

  # The text list is produced by the verifier itself, so the two sides cannot
  # disagree about which texts they are comparing.
  $verifier = Join-Path $repo "tools"
  $verifier = Join-Path $verifier "verify-stage6.mjs"
  $texts = & node $verifier --list-cli-cases
  if (-not $texts) { throw "verify-stage6.mjs --list-cli-cases produced nothing" }
  [IO.File]::WriteAllText((Join-Path $dump "cli-texts.txt"),
                          (($texts -join "`n") + "`n"),
                          (New-Object Text.UTF8Encoding $false))

  # Built in two statements on purpose: PowerShell binds the -replace operands
  # as further arguments to WriteAllText if the expression is written inline,
  # and the error it gives ("no overload ... argument count 3") names neither.
  $body = (($lines -join "`n") + "`n") -replace "`r`n", "`n"
  [IO.File]::WriteAllText($shFile, $body)
  wsl -d $WslDistro -- bash "$wslDump/dump.sh" | Out-Null

  # Stage 2 has no directory mode and needs none: it is pure table data, with
  # no arithmetic a second libm could answer differently.
  foreach ($st in ($stages | Where-Object { $_.Stage -ne 2 })) {
    Write-Host "--- build-wsl, stage $($st.Stage) ---"
    & node (Join-Path $repo "tools\$($st.Verify)") $dump | Select-Object -Last 4 | Write-Host
  }
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
