[CmdletBinding()]
param(
    [ValidateSet("Quick", "Tools", "Full")]
    [string]$Profile = "Quick",

    [string]$ProjectRoot = "",

    [string]$MsysRoot = "C:\msys64",

    [switch]$OfficialOptimizationSmoke,

    [ValidateSet("33", "40")]
    [string[]]$OfficialCases = @("33", "40"),

    [ValidateRange(1, 600)]
    [int]$OptimizationTimeLimitSeconds = 30,

    [ValidateRange(30, 900)]
    [int]$OfficialCaseTimeoutSeconds = 120,

    [switch]$KeepArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw "run_tools_regression.ps1 requires PowerShell 7 or newer."
}

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path

$toolsExecutable = Join-Path $ProjectRoot "tools.exe"
$bashExecutable = Join-Path $MsysRoot "usr\bin\bash.exe"
$powerShellExecutable = (Get-Process -Id $PID).Path
$runId = Get-Date -Format "yyyyMMdd-HHmmss"
$logDirectory = Join-Path $ProjectRoot "Testing\tools-regression\$runId"
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null

$script:results = [System.Collections.Generic.List[object]]::new()
$script:buildPassed = $true

function Convert-ToSafeFileName {
    param([Parameter(Mandatory = $true)][string]$Name)

    $safe = $Name -replace "[^A-Za-z0-9._-]", "_"
    return $safe.Trim("_")
}

function Add-StepResult {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [Parameter(Mandatory = $true)][double]$ElapsedSeconds,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [bool]$TimedOut = $false,
        [bool]$Skipped = $false
    )

    $script:results.Add([pscustomobject]@{
        Name = $Name
        Passed = $Passed
        TimedOut = $TimedOut
        Skipped = $Skipped
        ElapsedSeconds = $ElapsedSeconds
        LogPath = $LogPath
    })
}

function Show-FailureTail {
    param([Parameter(Mandatory = $true)][string]$LogPath)

    if (Test-Path -LiteralPath $LogPath) {
        Write-Host "       Last log lines:"
        Get-Content -LiteralPath $LogPath -Tail 40 |
            ForEach-Object { Write-Host "       $_" }
    }
}

function Invoke-ProcessStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [int]$TimeoutSeconds = 180,
        [hashtable]$Environment = @{},
        [AllowNull()][string]$StandardInputText = $null
    )

    $safeName = Convert-ToSafeFileName $Name
    $logPath = Join-Path $logDirectory "$safeName.log"
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $exitCode = -1
    $lastHeartbeatSecond = 0

    Write-Host ("[RUN] {0} (timeout {1}s)" -f $Name, $TimeoutSeconds)

    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $FilePath
        $startInfo.WorkingDirectory = $ProjectRoot
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.RedirectStandardInput = $null -ne $StandardInputText
        $startInfo.CreateNoWindow = $true

        foreach ($argument in $ArgumentList) {
            $startInfo.ArgumentList.Add($argument)
        }
        foreach ($entry in $Environment.GetEnumerator()) {
            $startInfo.Environment[$entry.Key] = [string]$entry.Value
        }

        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Failed to start process: $FilePath"
        }

        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if ($null -ne $StandardInputText) {
            $process.StandardInput.Write($StandardInputText)
            $process.StandardInput.Close()
        }
        while (-not $process.WaitForExit(1000)) {
            $elapsedSecond = [int][Math]::Floor($stopwatch.Elapsed.TotalSeconds)
            if ($elapsedSecond -ge $TimeoutSeconds) {
                $timedOut = $true
                try {
                    $process.Kill($true)
                } catch {
                    $process.Kill()
                }
                $process.WaitForExit()
                break
            }

            if (($elapsedSecond - $lastHeartbeatSecond) -ge 15) {
                Write-Host (
                    "[WAIT] {0} still running ({1}s elapsed, timeout {2}s)" -f
                    $Name,
                    $elapsedSecond,
                    $TimeoutSeconds)
                $lastHeartbeatSecond = $elapsedSecond
            }
        }

        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if (-not $timedOut) {
            $exitCode = $process.ExitCode
        }

        $logText = @(
            "STEP: $Name"
            "COMMAND: $FilePath $($ArgumentList -join ' ')"
            "TIMEOUT_SECONDS: $TimeoutSeconds"
            "TIMED_OUT: $($timedOut.ToString().ToLowerInvariant())"
            "EXIT_CODE: $exitCode"
            ""
            "STDOUT:"
            $stdout
            ""
            "STDERR:"
            $stderr
        ) -join [Environment]::NewLine
        [System.IO.File]::WriteAllText($logPath, $logText)
    } catch {
        [System.IO.File]::WriteAllText(
            $logPath,
            "STEP: $Name`nEXCEPTION:`n$($_ | Out-String)")
    } finally {
        $stopwatch.Stop()
    }

    $passed = -not $timedOut -and $exitCode -eq 0
    Add-StepResult `
        -Name $Name `
        -Passed $passed `
        -ElapsedSeconds $stopwatch.Elapsed.TotalSeconds `
        -LogPath $logPath `
        -TimedOut $timedOut

    if ($passed) {
        Write-Host ("[PASS] {0} ({1:N2}s)" -f $Name, $stopwatch.Elapsed.TotalSeconds)
    } elseif ($timedOut) {
        Write-Host ("[TIMEOUT] {0} ({1:N2}s)" -f $Name, $stopwatch.Elapsed.TotalSeconds)
        Show-FailureTail -LogPath $logPath
    } else {
        Write-Host ("[FAIL] {0} ({1:N2}s)" -f $Name, $stopwatch.Elapsed.TotalSeconds)
        Show-FailureTail -LogPath $logPath
    }

    return $passed
}

function Invoke-InternalStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )

    $safeName = Convert-ToSafeFileName $Name
    $logPath = Join-Path $logDirectory "$safeName.log"
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $passed = $false

    try {
        $output = & $Action | Out-String
        [System.IO.File]::WriteAllText($logPath, $output)
        $passed = $true
    } catch {
        [System.IO.File]::WriteAllText($logPath, ($_ | Out-String))
    } finally {
        $stopwatch.Stop()
    }

    Add-StepResult `
        -Name $Name `
        -Passed $passed `
        -ElapsedSeconds $stopwatch.Elapsed.TotalSeconds `
        -LogPath $logPath

    if ($passed) {
        Write-Host ("[PASS] {0} ({1:N2}s)" -f $Name, $stopwatch.Elapsed.TotalSeconds)
    } else {
        Write-Host ("[FAIL] {0} ({1:N2}s)" -f $Name, $stopwatch.Elapsed.TotalSeconds)
        Show-FailureTail -LogPath $logPath
    }

    return $passed
}

function Add-SkippedStep {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Reason
    )

    $logPath = Join-Path $logDirectory "$(Convert-ToSafeFileName $Name).log"
    [System.IO.File]::WriteAllText($logPath, "SKIPPED: $Reason")
    Add-StepResult `
        -Name $Name `
        -Passed $false `
        -ElapsedSeconds 0.0 `
        -LogPath $logPath `
        -Skipped $true
    Write-Host "[SKIP] $Name - $Reason"
}

function Invoke-PowerShellRegression {
    param([Parameter(Mandatory = $true)][string]$TestNumber)

    $testScript = Join-Path $ProjectRoot "mini test\test$TestNumber\test$TestNumber.ps1"
    if (-not (Test-Path -LiteralPath $testScript)) {
        throw "Missing regression script: $testScript"
    }

    return Invoke-ProcessStep `
        -Name "test$TestNumber" `
        -FilePath $powerShellExecutable `
        -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            $testScript,
            "-Executable",
            $toolsExecutable
        ) `
        -TimeoutSeconds 180
}

function Test-Environment {
    $requiredPaths = @(
        $bashExecutable,
        (Join-Path $ProjectRoot "TOOLS_SPEC\Makefile"),
        (Join-Path $ProjectRoot "include\lib\abc\libabc.a"),
        (Join-Path $ProjectRoot "include\lib\cadical\build\libcadical.a")
    )
    if ($Profile -eq "Quick") {
        $requiredPaths += $toolsExecutable
    }

    $missing = @($requiredPaths | Where-Object { -not (Test-Path -LiteralPath $_) })
    if ($missing.Count -gt 0) {
        throw "Missing required paths:`n$($missing -join [Environment]::NewLine)"
    }

    "profile=$Profile"
    "project_root=$ProjectRoot"
    "powershell=$($PSVersionTable.PSVersion)"
    "bash=$bashExecutable"
    "tools=$toolsExecutable"
}

function Test-ToolsSpecLinks {
    $broken = [System.Collections.Generic.List[string]]::new()
    Get-ChildItem -LiteralPath (Join-Path $ProjectRoot "TOOLS_SPEC") -Filter "*.md" |
        ForEach-Object {
            $document = $_
            $text = Get-Content -LiteralPath $document.FullName -Raw
            foreach ($match in [regex]::Matches($text, "\[[^\]]+\]\(([^)#]+\.md)\)")) {
                $target = Join-Path $document.DirectoryName $match.Groups[1].Value
                if (-not (Test-Path -LiteralPath $target)) {
                    $broken.Add("$($document.Name) -> $($match.Groups[1].Value)")
                }
            }
        }

    if ($broken.Count -gt 0) {
        throw "Broken TOOLS_SPEC links:`n$($broken -join [Environment]::NewLine)"
    }
    "checked_tools_spec_links=true"
}

function Test-StaleOptimizationStatus {
    $patterns = @(
        "tools CLI 待串接",
        "tools.cpp 尚未實作",
        "tools.cpp 尚未 expose",
        "tools.cpp / TOOL_SPEC 尚待串接",
        "future constrained optimizer",
        "剩 tools CLI 串接",
        "尚未串接 tools CLI"
    )
    $roots = @("API_SPEC", "TOOLS_SPEC", "PROJECT_SPEC") |
        ForEach-Object { Join-Path $ProjectRoot $_ }
    $matches = [System.Collections.Generic.List[string]]::new()

    Get-ChildItem -LiteralPath $roots -Filter "*.md" -Recurse |
        ForEach-Object {
            $file = $_
            foreach ($pattern in $patterns) {
                Select-String -LiteralPath $file.FullName -SimpleMatch $pattern |
                    ForEach-Object {
                        $relative = [System.IO.Path]::GetRelativePath(
                            $ProjectRoot,
                            $file.FullName)
                        $matches.Add("${relative}:$($_.LineNumber): $($_.Line.Trim())")
                    }
            }
        }

    if ($matches.Count -gt 0) {
        throw "Stale optimization status text found:`n$($matches -join [Environment]::NewLine)"
    }
    "stale_optimization_status=false"
}

function Test-OfficialOptimizationLog {
    param(
        [Parameter(Mandatory = $true)][string]$CaseName,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][int]$ExpectedResponseCount
    )

    $text = Get-Content -LiteralPath $LogPath -Raw
    $responses = @(
        [regex]::Matches(
            $text,
            "(?s)TOOL_RESULT_BEGIN.*?TOOL_RESULT_END") |
            ForEach-Object { $_.Value }
    )
    if ($responses.Count -ne $ExpectedResponseCount) {
        throw "$CaseName returned $($responses.Count) envelopes; expected $ExpectedResponseCount."
    }

    $failedResponses = @(
        $responses |
            Where-Object {
                $_ -notmatch "(?m)^ok: true\r?$" -or
                $_ -notmatch "(?m)^complete: true\r?$"
            }
    )
    if ($failedResponses.Count -gt 0) {
        throw "$CaseName contains $($failedResponses.Count) non-complete or failed tool responses."
    }

    $optimization = @(
        $responses |
            Where-Object {
                $_ -match "(?m)^command: opt_apply\r?$" -and
                $_ -match "(?m)^mode: critical_path_depth\r?$"
            }
    )
    if ($optimization.Count -ne 1) {
        throw "$CaseName must contain exactly one critical_path_depth opt_apply response."
    }
    $optimizationReport = $optimization[0]
    $requiredOptimizationPatterns = @(
        "(?m)^\s+report_success: true\r?$",
        "(?m)^\s+rolled_back: false\r?$",
        "(?m)^\s+functionally_equivalent: true\r?$",
        "(?m)^\s+objective_metric: scoped_fanin_cone_depth\r?$",
        "(?m)^\s+final_constraints_satisfied: true\r?$",
        "(?m)^\s+whole_design_timed_out: false\r?$"
    )
    foreach ($pattern in $requiredOptimizationPatterns) {
        if ($optimizationReport -notmatch $pattern) {
            throw "$CaseName optimization report is missing required pattern: $pattern"
        }
    }

    $expectedScopeName = if ($CaseName -eq "test33") { "n8" } else { "n14" }
    if ($optimizationReport -notmatch
        "(?m)^\s+requested_scope_name: $expectedScopeName\r?$") {
        throw "$CaseName optimization report does not target $expectedScopeName."
    }
    if ($optimizationReport -notmatch
        "(?s)allowed_gate_types \(2\):.*\bNAND\b.*\bNOT\b") {
        throw "$CaseName optimization report does not record NAND/NOT constraints."
    }

    if ($CaseName -eq "test40") {
        $summary = @(
            $responses |
                Where-Object {
                    $_ -match "(?m)^command: structure_query\r?$" -and
                    $_ -match "(?m)^mode: summary\r?$"
                }
        )
        if ($summary.Count -ne 1) {
            throw "test40 must contain one final structure_query summary."
        }
        if ($summary[0] -match
            "(?m)^\s+(AND|OR|NOR|XOR|XNOR|BUF)\s*:\s*[1-9][0-9]*\r?$") {
            throw "test40 final design contains a gate outside the NAND/NOT basis."
        }
    }

    "$CaseName envelopes=$($responses.Count)"
    "$CaseName optimization_complete=true"
}

function Invoke-OfficialOptimizationSmoke {
    $cases = @(
        [pscustomobject]@{
            Name = "test33"
            ExpectedResponses = 9
            Commands = @(
                "read NewTestCase/test33/test33.v"
                "edit_apply replace_type whole XNOR -allow NOR"
                "edit_apply remove_dangling_logic"
                "edit_apply merge_structurally_equivalent_gates"
                "depth_query net n8"
                "opt_apply critical_path_depth --scope net_fanin n8 --objective cone --allowed NAND NOT --time-limit $OptimizationTimeLimitSeconds"
                "report_query last_edit"
                "depth_query net n8"
                "quit"
            )
        },
        [pscustomobject]@{
            Name = "test40"
            ExpectedResponses = 11
            Commands = @(
                "read NewTestCase/test40/test40.v"
                "edit_apply convert_basis whole -allow NAND NOT"
                "edit_apply simplify_constants NAND 1 --inputs 2"
                "edit_apply collapse_double_inverter"
                "depth_query net n14"
                "opt_apply critical_path_depth --scope net_fanin n14 --objective cone --allowed NAND NOT --time-limit $OptimizationTimeLimitSeconds"
                "report_query last_edit"
                "depth_query net n14"
                "depth_query global_critical"
                "structure_query summary"
                "quit"
            )
        }
    )

    foreach ($case in $cases | Where-Object {
            $OfficialCases -contains $_.Name.Substring(4)
        }) {
        $stepName = "official_$($case.Name)"
        $inputText = ($case.Commands -join [Environment]::NewLine) +
            [Environment]::NewLine
        $processPassed = Invoke-ProcessStep `
            -Name $stepName `
            -FilePath $toolsExecutable `
            -TimeoutSeconds $OfficialCaseTimeoutSeconds `
            -StandardInputText $inputText
        if (-not $processPassed) {
            continue
        }

        $caseLogPath = Join-Path $logDirectory "$stepName.log"
        $caseName = $case.Name
        $expectedResponses = $case.ExpectedResponses
        [void](Invoke-InternalStep `
            -Name "${stepName}_validate" `
            -Action {
                Test-OfficialOptimizationLog `
                    -CaseName $caseName `
                    -LogPath $caseLogPath `
                    -ExpectedResponseCount $expectedResponses
            })
    }
}

function Remove-RegressionArtifacts {
    $artifacts = @(
        (Join-Path $ProjectRoot "Testing\test9\tools_session_output.v"),
        (Join-Path $ProjectRoot "mini test\test31\test31.exe")
    )

    foreach ($artifact in $artifacts) {
        if (-not (Test-Path -LiteralPath $artifact)) {
            continue
        }
        $resolved = (Resolve-Path -LiteralPath $artifact).Path
        if (-not $resolved.StartsWith(
                $ProjectRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove artifact outside project root: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Force
    }
}

Write-Host "Tools regression profile: $Profile"
Write-Host "Logs: $logDirectory"

try {
    [void](Invoke-InternalStep -Name "environment" -Action ${function:Test-Environment})

    if ($Profile -ne "Quick") {
        $script:buildPassed = Invoke-ProcessStep `
            -Name "build_tools" `
            -FilePath $bashExecutable `
            -ArgumentList @("-lc", "make -f TOOLS_SPEC/Makefile -j4") `
            -TimeoutSeconds 300 `
            -Environment @{
                MSYSTEM = "UCRT64"
                CHERE_INVOKING = "1"
            }
    }

    if ($script:buildPassed -and (Test-Path -LiteralPath $toolsExecutable)) {
        $testNumbers = if ($Profile -eq "Quick") {
            @("32")
        } elseif ($Profile -eq "Tools") {
            @("9", "21", "27", "32")
        } else {
            @(
                "9", "10", "11", "12", "13", "14", "15", "16", "17", "18",
                "21", "22", "24", "25", "26", "27", "32"
            )
        }

        foreach ($testNumber in $testNumbers) {
            [void](Invoke-PowerShellRegression -TestNumber $testNumber)
        }
    } else {
        Add-SkippedStep `
            -Name "cli_regressions" `
            -Reason "tools.exe is unavailable because the build failed."
    }

    if ($Profile -eq "Full") {
        if ($script:buildPassed) {
            $test31Command = @'
set -e
sources=$(find src/core src/io src/analysis src/optimization src/transformation include/lib/abcsat include/lib/abcesop -name "*.cpp" -print)
g++ -DFMT_HEADER_ONLY -DWIN64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -DABC_USE_STDINT_H -DNUNLOCKED -DFMT_USE_WINDOWS_H=0 \
    -I. -Iinclude -Iinclude/lib -Iinclude/lib/nauty \
    -Iinclude/lib/abcsat -Iinclude/lib/abcesop \
    -Iinclude/lib/cadical/src -Iinclude/lib/abc/src \
    -std=c++20 -fpermissive -O3 \
    -Wno-unknown-pragmas -Wno-narrowing -Wno-sign-compare \
    -Wno-unused-parameter -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-format \
    -Wno-misleading-indentation -Wno-dangling-else \
    -Wno-class-memaccess -Wno-int-to-pointer-cast \
    "mini test/test31/test31.cpp" $sources \
    -Wl,--start-group include/lib/abc/libabc.a \
    include/lib/cadical/build/libcadical.a -Wl,--end-group \
    -lpthread -lm -o "mini test/test31/test31.exe"
"mini test/test31/test31.exe"
'@
            [void](Invoke-ProcessStep `
                -Name "test31_cpp" `
                -FilePath $bashExecutable `
                -ArgumentList @("-lc", $test31Command) `
                -TimeoutSeconds 300 `
                -Environment @{
                    MSYSTEM = "UCRT64"
                    CHERE_INVOKING = "1"
                })
        } else {
            Add-SkippedStep `
                -Name "test31_cpp" `
                -Reason "tools build failed; shared source baseline is not valid."
        }
    }

    if ($OfficialOptimizationSmoke) {
        if ($script:buildPassed -and (Test-Path -LiteralPath $toolsExecutable)) {
            Invoke-OfficialOptimizationSmoke
        } else {
            Add-SkippedStep `
                -Name "official_optimization_smoke" `
                -Reason "tools.exe is unavailable because the build failed."
        }
    }

    [void](Invoke-ProcessStep `
        -Name "git_diff_check" `
        -FilePath "git" `
        -ArgumentList @("diff", "--check") `
        -TimeoutSeconds 60)
    [void](Invoke-InternalStep `
        -Name "tools_spec_links" `
        -Action ${function:Test-ToolsSpecLinks})
    [void](Invoke-InternalStep `
        -Name "stale_optimization_status" `
        -Action ${function:Test-StaleOptimizationStatus})
} finally {
    if (-not $KeepArtifacts) {
        Remove-RegressionArtifacts
    }
}

$passedCount = @($script:results | Where-Object { $_.Passed }).Count
$failedCount = @(
    $script:results |
        Where-Object { -not $_.Passed -and -not $_.Skipped }
).Count
$skippedCount = @($script:results | Where-Object { $_.Skipped }).Count
$elapsedSeconds = (
    $script:results |
        Measure-Object -Property ElapsedSeconds -Sum
).Sum

Write-Host ""
Write-Host "Regression summary"
Write-Host "  Profile: $Profile"
Write-Host "  Passed: $passedCount"
Write-Host "  Failed: $failedCount"
Write-Host "  Skipped: $skippedCount"
Write-Host ("  Step time: {0:N2}s" -f $elapsedSeconds)
Write-Host "  Logs: $logDirectory"

if ($failedCount -ne 0 -or $skippedCount -ne 0) {
    exit 1
}
exit 0
