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
$passed = 0
$failedTests = @()

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

    [string[]]$rtList = if ($RtObjs) { $RtObjs -split '\s+' } else { @() }
    $testOk = $true

    # ── Execute based on mode ──
    if ($TestMode -eq 'normal') {
        & $Resurg $testFile -o "$Build/_test.c"
        if ($LASTEXITCODE -eq 0) {
            & $CC -std=c17 -Wno-tautological-compare "-I$Runtime" -o "$Build/_test.exe" "$Build/_test.c" @rtList
        }
        if ($LASTEXITCODE -eq 0) {
            & "$Build/_test.exe"
        }
        if ($LASTEXITCODE -ne 0) { $testOk = $false }
    }
    elseif ($TestMode -eq 'compile_error') {
        $stderrFile = "$Build/_test_stderr.txt"
        $proc = Start-Process -FilePath $Resurg -ArgumentList $testFile,'-o',"$Build/_test.c" `
            -RedirectStandardError $stderrFile -NoNewWindow -Wait -PassThru
        $exitCode = $proc.ExitCode
        $stderr = if (Test-Path $stderrFile) { Get-Content -Raw $stderrFile } else { '' }
        if ($exitCode -eq 0) {
            Write-Host "  FAIL  $testFile — expected compile error but resurg succeeded"
            $testOk = $false
        }
        elseif ($ExpectError -ne '' -and $stderr -notlike "*$ExpectError*") {
            Write-Host "  FAIL  $testFile — expected error: $ExpectError"
            Write-Host "         got: $stderr"
            $testOk = $false
        }
    }
    elseif ($TestMode -eq 'runtime_error') {
        & $Resurg $testFile -o "$Build/_test.c"
        if ($LASTEXITCODE -eq 0) {
            & $CC -std=c17 -Wno-tautological-compare "-I$Runtime" -o "$Build/_test.exe" "$Build/_test.c" @rtList
        }
        if ($LASTEXITCODE -ne 0) {
            $testOk = $false
        }
        else {
            $savedEAP = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            & "$Build/_test.exe" 2>$null
            $ErrorActionPreference = $savedEAP
            if ($LASTEXITCODE -eq 0) {
                Write-Host "  FAIL  $testFile — expected runtime error but program succeeded"
                $testOk = $false
            }
        }
    }
    else {
        Write-Host "  FAIL  $testFile — unknown test mode: $TestMode"
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
        Write-Host "  FAIL  $f"
    }
    exit 1
}
