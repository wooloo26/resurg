# run_all_tests.ps1 — Run all .rsg tests in a single process (avoids per-test
# PowerShell startup overhead).
#
# Usage: run_all_tests.ps1 <resurg> <cc> <rt_objs> <build> <runtime> <test>...
param(
    [Parameter(Mandatory)][string]$Resurg,
    [Parameter(Mandatory)][string]$CC,
    [Parameter(Mandatory)][string]$RtObjs,
    [Parameter(Mandatory)][string]$Build,
    [Parameter(Mandatory)][string]$Runtime,
    [Parameter(ValueFromRemainingArguments)][string[]]$TestFiles
)

$ErrorActionPreference = 'Stop'
$Timeout = if ($env:RSG_TEST_TIMEOUT) { [int]$env:RSG_TEST_TIMEOUT } else { 10 }
$passed = 0
$failedTests = @()

# -----------------------------------------------------------------------
# Colored output helpers
# -----------------------------------------------------------------------
function Fail($File, $Msg, $Cmd, $Output) {
    Write-Host -ForegroundColor Red "  FAIL  $File — $Msg"
    if ($Cmd)    { Write-Host -ForegroundColor Red "         cmd: $Cmd" }
    if ($Output) { Write-Host -ForegroundColor Red "         output: $Output" }
}

function Describe-Exit($Code) {
    if ($Code -gt 128) {
        $sig = $Code - 128
        switch ($sig) {
            6  { return 'SIGABRT (abort)' }
            8  { return 'SIGFPE (floating point exception)' }
            9  { return 'SIGKILL (killed)' }
            11 { return 'SIGSEGV (segmentation fault)' }
            default { return "signal $sig" }
        }
    }
    return "exit code $Code"
}

foreach ($testFile in $TestFiles) {
    # ── Parse directives from leading comments ──
    $TestMode = 'normal'
    $ExpectError = ''
    foreach ($line in (Get-Content -LiteralPath $testFile)) {
        if ($line -match '^//\s+TEST:\s+(.+)') { $TestMode = $Matches[1].Trim() }
        elseif ($line -match '^//\s+EXPECT-ERROR:\s+(.+)') { $ExpectError = $Matches[1].Trim() }
        elseif ($line -match '^//' -or $line -eq '') { continue }
        else { break }
    }

    # ── Output paths mirror test file layout ──
    $RelPath = $testFile -replace '\.rsg$', ''
    $TestC   = "$Build/$RelPath.c"
    $TestBin = "$Build/$RelPath.exe"
    $OutDir  = Split-Path -Parent $TestC
    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

    [string[]]$rtList = if ($RtObjs) { $RtObjs -split '\s+' } else { @() }
    $testOk = $true

    # ── Execute based on mode ──
    if ($TestMode -eq 'normal') {
        $stderr = & $Resurg $testFile -o $TestC 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Fail $testFile "resurg codegen failed ($(Describe-Exit $LASTEXITCODE))" "$Resurg $testFile -o $TestC" $stderr
            $testOk = $false
        }
        if ($testOk) {
            $stderr = & $CC -std=c17 -Wno-tautological-compare "-I$Runtime" -o $TestBin $TestC @rtList 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) {
                Fail $testFile 'C compilation failed' "$CC ... $TestC" $stderr
                $testOk = $false
            }
        }
        if ($testOk) {
            $proc = Start-Process -FilePath $TestBin -NoNewWindow -Wait -PassThru -RedirectStandardError "$OutDir/_stderr.txt"
            if ($proc.ExitCode -ne 0) {
                $errout = if (Test-Path "$OutDir/_stderr.txt") { Get-Content -Raw "$OutDir/_stderr.txt" } else { '' }
                Remove-Item -Force -ErrorAction SilentlyContinue "$OutDir/_stderr.txt"
                if ($proc.ExitCode -eq 124) {
                    Fail $testFile "test binary timed out (possible infinite loop, ${Timeout}s limit)" $TestBin
                } else {
                    Fail $testFile "test binary crashed ($(Describe-Exit $proc.ExitCode))" $TestBin $errout
                }
                $testOk = $false
            }
            Remove-Item -Force -ErrorAction SilentlyContinue "$OutDir/_stderr.txt"
        }
    }
    elseif ($TestMode -eq 'compile_error') {
        $stderrFile = "$OutDir/_stderr.txt"
        $proc = Start-Process -FilePath $Resurg -ArgumentList $testFile,'-o',$TestC `
            -RedirectStandardError $stderrFile -NoNewWindow -Wait -PassThru
        $exitCode = $proc.ExitCode
        $stderr = if (Test-Path $stderrFile) { Get-Content -Raw $stderrFile } else { '' }
        Remove-Item -Force -ErrorAction SilentlyContinue $stderrFile
        if ($exitCode -eq 0) {
            Fail $testFile 'expected compile error but resurg succeeded'
            $testOk = $false
        }
        elseif ($ExpectError -ne '' -and $stderr -notlike "*$ExpectError*") {
            Fail $testFile "expected error: $ExpectError" "$Resurg $testFile" "got: $stderr"
            $testOk = $false
        }
    }
    elseif ($TestMode -eq 'runtime_error') {
        $stderr = & $Resurg $testFile -o $TestC 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Fail $testFile "resurg codegen failed ($(Describe-Exit $LASTEXITCODE))" "$Resurg $testFile -o $TestC" $stderr
            $testOk = $false
        }
        if ($testOk) {
            $stderr = & $CC -std=c17 -Wno-tautological-compare "-I$Runtime" -o $TestBin $TestC @rtList 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0) {
                Fail $testFile 'C compilation failed' "$CC ... $TestC" $stderr
                $testOk = $false
            }
        }
        if ($testOk) {
            $savedEAP = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            & $TestBin 2>$null
            $ErrorActionPreference = $savedEAP
            if ($LASTEXITCODE -eq 0) {
                Fail $testFile 'expected runtime error but program succeeded'
                $testOk = $false
            }
        }
    }
    else {
        Fail $testFile "unknown test mode: $TestMode"
        $testOk = $false
    }

    if ($testOk) {
        Write-Host "  PASS  $testFile"
        $passed++
    } else {
        $failedTests += $testFile
    }
}

Write-Host "$passed tests passed."
if ($failedTests.Count -gt 0) {
    foreach ($f in $failedTests) {
        Write-Host -ForegroundColor Red "  FAIL  $f"
    }
    exit 1
}
