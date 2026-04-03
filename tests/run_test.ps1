# run_test.ps1 — Execute a single .rsg test with directive support.
#
# Usage: run_test.ps1 <file.rsg> <resurg> <cc> <rt_objs> <build> <runtime>
#
# Directives (parsed from leading comments):
#   // TEST: compile_error   — resurg must exit != 0
#   // TEST: runtime_error   — binary must exit != 0
#   // EXPECT-ERROR: <text>  — stderr must contain <text>
param(
    [Parameter(Mandatory)][string]$RsgFile,
    [Parameter(Mandatory)][string]$Resurg,
    [Parameter(Mandatory)][string]$CC,
    [Parameter(Mandatory)][string]$RtObjs,
    [Parameter(Mandatory)][string]$Build,
    [Parameter(Mandatory)][string]$Runtime
)

$ErrorActionPreference = 'Stop'

# -----------------------------------------------------------------------
# Parse directives from leading comments
# -----------------------------------------------------------------------
$TestMode = 'normal'
$ExpectError = ''

foreach ($line in (Get-Content -LiteralPath $RsgFile)) {
    if ($line -match '^//\s+TEST:\s+(.+)') {
        $TestMode = $Matches[1].Trim()
    } elseif ($line -match '^//\s+EXPECT-ERROR:\s+(.+)') {
        $ExpectError = $Matches[1].Trim()
    } elseif ($line -match '^//' -or $line -eq '') {
        continue
    } else {
        break
    }
}

# -----------------------------------------------------------------------
# Execute based on mode
# -----------------------------------------------------------------------
switch ($TestMode) {
    'normal' {
        & $Resurg $RsgFile -o "$Build/_test.c"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        [string[]]$rtList = if ($RtObjs) { $RtObjs -split '\s+' } else { @() }
        & $CC -std=c17 "-I$Runtime" -o "$Build/_test.exe" "$Build/_test.c" @rtList
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        & "$Build/_test.exe"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    'compile_error' {
        $stderrFile = "$Build/_test_stderr.txt"
        $proc = Start-Process -FilePath $Resurg -ArgumentList "$RsgFile",'-o',"$Build/_test.c" `
            -RedirectStandardError $stderrFile -NoNewWindow -Wait -PassThru
        $exitCode = $proc.ExitCode
        $stderr = if (Test-Path $stderrFile) { Get-Content -Raw $stderrFile } else { '' }

        if ($exitCode -eq 0) {
            Write-Host "  FAIL  $RsgFile — expected compile error but resurg succeeded"
            exit 1
        }
        if ($ExpectError -ne '' -and $stderr -notlike "*$ExpectError*") {
            Write-Host "  FAIL  $RsgFile — expected error: $ExpectError"
            Write-Host "         got: $stderr"
            exit 1
        }
    }

    'runtime_error' {
        & $Resurg $RsgFile -o "$Build/_test.c"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        [string[]]$rtList = if ($RtObjs) { $RtObjs -split '\s+' } else { @() }
        & $CC -std=c17 "-I$Runtime" -o "$Build/_test.exe" "$Build/_test.c" @rtList
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        $ErrorActionPreference = 'Continue'
        & "$Build/_test.exe" 2>$null
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  FAIL  $RsgFile — expected runtime error but program succeeded"
            exit 1
        }
    }

    default {
        Write-Host "  FAIL  $RsgFile — unknown test mode: $TestMode"
        exit 1
    }
}
