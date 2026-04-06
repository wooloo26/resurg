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
$Timeout = if ($env:RSG_TEST_TIMEOUT) { [int]$env:RSG_TEST_TIMEOUT } else { 10 }

# -----------------------------------------------------------------------
# Colored output helpers
# -----------------------------------------------------------------------
function Fail($Msg, $Cmd, $Output) {
    Write-Host -ForegroundColor Red "  FAIL  $RsgFile — $Msg"
    if ($Cmd)    { Write-Host -ForegroundColor Red "         cmd: $Cmd" }
    if ($Output) { Write-Host -ForegroundColor Red "         output: $Output" }
    exit 1
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
# Output paths mirror test file layout inside the build directory.
# e.g. tests/integration/v0.1.0/foo.rsg → build/tests/integration/v0.1.0/foo.c
# -----------------------------------------------------------------------
$RelPath = $RsgFile -replace '\.rsg$', ''
$TestC   = "$Build/$RelPath.c"
$TestBin = "$Build/$RelPath.exe"
$OutDir  = Split-Path -Parent $TestC
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

# -----------------------------------------------------------------------
# Execute based on mode
# -----------------------------------------------------------------------
switch ($TestMode) {
    'normal' {
        $stderr = & $Resurg $RsgFile -o $TestC 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Fail "resurg codegen failed ($(Describe-Exit $LASTEXITCODE))" "$Resurg $RsgFile -o $TestC" $stderr
        }

        [string[]]$rtList = if ($RtObjs) { $RtObjs -split '\s+' } else { @() }
        $stderr = & $CC -std=c17 -Wno-tautological-compare "-I$Runtime" -o $TestBin $TestC @rtList 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Fail 'C compilation failed' "$CC ... $TestC" $stderr
        }

        $proc = Start-Process -FilePath $TestBin -NoNewWindow -Wait -PassThru -RedirectStandardError "$OutDir/_stderr.txt"
        if ($proc.ExitCode -ne 0) {
            $errout = if (Test-Path "$OutDir/_stderr.txt") { Get-Content -Raw "$OutDir/_stderr.txt" } else { '' }
            Remove-Item -Force -ErrorAction SilentlyContinue "$OutDir/_stderr.txt"
            if ($proc.ExitCode -eq 124) {
                Fail "test binary timed out (possible infinite loop, ${Timeout}s limit)" $TestBin
            }
            Fail "test binary crashed ($(Describe-Exit $proc.ExitCode))" $TestBin $errout
        }
        Remove-Item -Force -ErrorAction SilentlyContinue "$OutDir/_stderr.txt"
    }

    'compile_error' {
        $stderrFile = "$OutDir/_stderr.txt"
        $proc = Start-Process -FilePath $Resurg -ArgumentList $RsgFile,'-o',$TestC `
            -RedirectStandardError $stderrFile -NoNewWindow -Wait -PassThru
        $exitCode = $proc.ExitCode
        $stderr = if (Test-Path $stderrFile) { Get-Content -Raw $stderrFile } else { '' }
        Remove-Item -Force -ErrorAction SilentlyContinue $stderrFile

        if ($exitCode -eq 0) {
            Fail 'expected compile error but resurg succeeded'
        }
        if ($ExpectError -ne '' -and $stderr -notlike "*$ExpectError*") {
            Fail "expected error: $ExpectError" "$Resurg $RsgFile" "got: $stderr"
        }
    }

    'runtime_error' {
        $stderr = & $Resurg $RsgFile -o $TestC 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Fail "resurg codegen failed ($(Describe-Exit $LASTEXITCODE))" "$Resurg $RsgFile -o $TestC" $stderr
        }

        [string[]]$rtList = if ($RtObjs) { $RtObjs -split '\s+' } else { @() }
        $stderr = & $CC -std=c17 -Wno-tautological-compare "-I$Runtime" -o $TestBin $TestC @rtList 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) {
            Fail 'C compilation failed' "$CC ... $TestC" $stderr
        }

        $savedEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $TestBin 2>$null
        $ErrorActionPreference = $savedEAP
        if ($LASTEXITCODE -eq 0) {
            Fail 'expected runtime error but program succeeded'
        }
    }

    default {
        Fail "unknown test mode: $TestMode"
    }
}
