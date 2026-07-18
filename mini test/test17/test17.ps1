param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test17/functional_dependence_circuit.v"
    "func_query depends_on dependent a"
    "func_query depends_on cancelled a"
    "func_query depends_on absent a"
    "func_query depends_on multi a"
    "func_query depends_on dependent internal_signal"
    "func_query depends_on dependent missing_input"
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

Check-Result `
    ($output -match "(?s)net A: dependent.*?net B: a.*?status: FUNCTIONALLY_DEPENDENT.*?solver status: SAT.*?depends on input: yes") `
    "SAT proves ordinary Boolean dependence"
Check-Result `
    ($output -match "(?s)net A: cancelled.*?net B: a.*?status: FUNCTIONALLY_INDEPENDENT.*?solver status: UNSAT.*?input in structural support: yes.*?depends on input: no") `
    "UNSAT distinguishes functional cancellation from structural support"
Check-Result `
    ($output -match "(?s)net A: absent.*?net B: a.*?status: FUNCTIONALLY_INDEPENDENT.*?input in structural support: no.*?depends on input: no") `
    "input outside the cone is rejected without a false dependence"
Check-Result `
    ($output -match "(?s)net A: multi.*?net B: a.*?status: FUNCTIONALLY_DEPENDENT.*?depends on input: yes") `
    "arbitrary-fanin AND is supported by dependence CNF"
Check-Result `
    ($output.Contains("input must be a primary input or DFF.Q pseudo-primary input")) `
    "internal signal cannot be treated as an independent input"
Check-Result `
    ($output.Contains("FunctionalDependence requires existing scalar target and input nets")) `
    "missing input is reported as invalid instead of independent"
Check-Result `
    (([regex]::Matches($output, "status: unsupported")).Count -eq 1) `
    "only the internal-input request is unsupported"
Check-Result `
    (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 8 -and `
     ([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 8) `
    "every command has one complete response envelope"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
