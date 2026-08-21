param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read NewTestCase/test65/test65.v"
    "func_query boolean_expression n30"
    "quit"
) -join "`n"

$output = $commands | & $Executable 2>&1 | Out-String
$passed = 0
$failed = 0

function Check-Result {
    param([bool]$Condition, [string]$Name)
    if ($Condition) {
        $script:passed++
        Write-Output "[PASS] $Name"
    } else {
        $script:failed++
        Write-Output "[FAIL] $Name"
    }
}

$artifactMatch = [regex]::Match($output, '(?m)^\s*output_file:\s*(.+\.txt)\s*$')
$artifactPath = if ($artifactMatch.Success) {
    Join-Path (Get-Location) $artifactMatch.Groups[1].Value.Trim()
} else {
    ""
}
$artifact = if ($artifactPath -and (Test-Path -LiteralPath $artifactPath)) {
    Get-Content -LiteralPath $artifactPath -Raw
} else {
    ""
}

Check-Result `
    ($output -match '(?s)mode: boolean_expression.*?complete: true.*?status: BOOLEAN_EQUATION_ARTIFACT') `
    "official test65 n30 Boolean query completes"
Check-Result `
    ($output.Contains("equation count: 0") -and $output.Contains("boundary count: 1")) `
    "DFF.Q target has no combinational equation and one boundary"
Check-Result `
    ($output.Contains("real primary input count: 0") -and
     $output.Contains("DFF.Q boundary count: 1") -and
     $output.Contains("undriven boundary count: 0")) `
    "tool separates real PI, DFF.Q, and undriven boundaries"
Check-Result `
    ($output.Contains("primary-input-only combinational expression available: no")) `
    "tool rejects a top-level-PI-only current-frame interpretation"
Check-Result `
    ($artifact.Contains("Target: n30") -and
     $artifact.Contains("n30 : DFF_Q") -and
     $artifact.Contains("state_variable=state_q0") -and
     $artifact.Contains("state_q0 = g3869.Q (net n30)") -and
     $artifact.Contains("Root: n30 (current-state variable state_q0)") -and
     $artifact.Contains("Equation count: 0") -and
     $artifact.Contains("Complete: yes")) `
    "artifact maps n30 to an explicit DFF current-state variable"
Check-Result `
    (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 3 -and
     ([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 3) `
    "every command has one response envelope"

if ($artifactPath -and (Test-Path -LiteralPath $artifactPath)) {
    Remove-Item -LiteralPath $artifactPath -Force
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
