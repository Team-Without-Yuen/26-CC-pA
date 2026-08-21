param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test58/functional_enable_hold.v"
    "sequential_query enable_hold all --summary-only"
    "sequential_query enable_hold ff_canonical"
    "sequential_query enable_hold ff_xor"
    "sequential_query enable_hold ff_derived"
    "sequential_query enable_hold ff_nand"
    "sequential_query enable_hold ff_nor"
    "sequential_query enable_hold ff_data_gate"
    "sequential_query enable_hold ff_hold"
    "sequential_query enable_hold ff_toggle"
    "sequential_query enable_hold ff_cross"
    "quit"
) -join "`n"

$output = $commands | & $Executable 2>&1 | Out-String
$responses = @(
    [regex]::Matches($output, "(?s)TOOL_RESULT_BEGIN.*?TOOL_RESULT_END") |
        ForEach-Object { $_.Value }
)
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
    ($responses.Count -eq 12) `
    "every command returns one envelope"
Check-Result `
    ($responses[1] -match "complete: true" -and
     $responses[1] -match "total_dff_count: 9" -and
     $responses[1] -match "matched_dff_count: 5") `
    "all five real enable-hold DFFs are counted"
Check-Result `
    ($responses[2] -match "dff_name: ff_canonical" -and
     $responses[2] -match "confirmed: true" -and
     $responses[2] -match "enable_net_name: en") `
    "canonical MUX is functionally proven"
Check-Result `
    ($responses[3] -match "dff_name: ff_xor" -and
     $responses[3] -match "detection_method: FunctionalCofactorSat" -and
     $responses[3] -match "confirmed: true") `
    "XOR-restructured MUX is functionally proven"
Check-Result `
    ($responses[4] -match "dff_name: ff_derived" -and
     $responses[4] -match "confirmed: true" -and
     $responses[4] -match "enable_net_name:\s*\r?\n") `
    "derived enable function is recognized without a named enable net"
Check-Result `
    ($responses[5] -match "dff_name: ff_nand" -and
     $responses[5] -match "confirmed: true") `
    "NAND/NOT restructuring is recognized functionally"
Check-Result `
    ($responses[6] -match "dff_name: ff_nor" -and
     $responses[6] -match "confirmed: true") `
    "NOR/NOT restructuring is recognized functionally"
Check-Result `
    ($responses[7] -match "DATA_GATING_WITHOUT_HOLD_FEEDBACK" -and
     $responses[7] -match "matched: false") `
    "data gating without Q feedback is rejected"
Check-Result `
    ($responses[8] -match "dff_name: ff_hold" -and
     $responses[8] -match "matched: false") `
    "permanent hold is rejected"
Check-Result `
    ($responses[9] -match "dff_name: ff_toggle" -and
     $responses[9] -match "matched: false") `
    "toggle feedback is rejected"
Check-Result `
    ($responses[10] -match "dff_name: ff_cross" -and
     $responses[10] -match "matched: false") `
    "another DFF Q cannot serve as same-DFF hold feedback"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}
exit 0
