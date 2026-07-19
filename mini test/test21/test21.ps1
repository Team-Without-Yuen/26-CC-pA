param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test21/sequential_cli_circuit.v"
    "sequential_query enable_hold all --summary-only"
    "sequential_query enable_hold all --confirmed-only"
    "sequential_query enable_hold all --limit 1"
    "sequential_query enable_hold all --offset 1 --limit 1"
    "sequential_query enable_hold ff_high --verify-sat"
    "sequential_query enable_hold ff_and"
    "sequential_query enable_hold ff_plain"
    "sequential_query enable_hold missing_ff"
    "sequential_query enable_hold all --verify-sat"
    "sequential_query enable_hold all --unknown-option"
    "help"
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
    ($output -match "(?s)command: sequential_query.*?matched_dff_count: 2.*?candidate_dff_count: 3.*?reported_dff_count: 0.*?omitted_no_pattern_count: 1") `
    "summary-only reports aggregate confirmed and candidate counts"
Check-Result `
    ($output -match "(?s)candidate_dff_count: 2.*?reported_dff_count: 2.*?dff_name: ff_high.*?dff_name: ff_low") `
    "confirmed-only excludes AND candidates and no-pattern DFFs"
Check-Result `
    ($output -match "(?s)available_dff_record_count: 3.*?reported_dff_count: 1.*?record_limit: 1.*?records_truncated: true.*?next_record_offset: 1") `
    "all-DFF detail pagination is explicit and bounded"
Check-Result `
    ($output -match "(?s)record_offset: 1.*?record_limit: 1.*?next_record_offset: 2.*?dff_name: ff_low") `
    "pagination offset resumes at the correct DFF record"
Check-Result `
    ($output -match "(?s)dff_name: ff_high.*?kind: MuxHold.*?active_level_name: active_high.*?detection_method: StructuralCanonicalWithSat.*?solver_status: PROVEN.*?hold_functionally_proven: true.*?load_functionally_proven: true") `
    "specific DFF optional SAT proof is fully exposed"
Check-Result `
    ($output -match "(?s)dff_name: ff_and.*?status: CANDIDATE_FOUND.*?kind: AndGatedDataCandidate.*?confirmed: false.*?semantics_pending: true") `
    "AND-only structure remains an explicit semantics-pending candidate"
Check-Result `
    ($output -match "(?s)dff_name: ff_plain.*?status: NO_PATTERN.*?matched: false.*?pattern_count: 0") `
    "specific no-pattern DFF still returns a detailed record"
Check-Result ($output.Contains("DFF not found: missing_ff")) "missing DFF is an explicit error"
Check-Result `
    ($output.Contains("--verify-sat requires a specific DFF target")) `
    "all-DFF SAT verification is rejected before expensive execution"
Check-Result `
    ($output.Contains("Unknown sequential_query option: --unknown-option")) `
    "unknown sequential option is rejected"
Check-Result `
    ($output.Contains("sequential_query enable_hold <all|dff_name>")) `
    "help exposes the sequential high-level command"
Check-Result `
    (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 13 -and `
     ([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 13) `
    "every sequential CLI request has one response envelope"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
