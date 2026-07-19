param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test12/tools_equiv_circuit.v"
    "equiv_query previous_edit 5"
    "equiv_query original 5"
    "edit_apply cleanup_buffers"
    "equiv_query previous_edit 5"
    "equiv_query original 5"
    "read mini test/test12/tools_equiv_circuit.v"
    "edit_apply rename_net y y_renamed"
    "equiv_query original 5"
    "equiv_query original 0"
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

Check-Result ($output.Contains("NO_PREVIOUS_EDIT_BASELINE")) "previous_edit requires an edit baseline"
Check-Result (([regex]::Matches($output, "(?m)^  equivalent: true\r?$")).Count -eq 3) "original and previous safe designs are equivalent"
Check-Result (([regex]::Matches($output, "method: WholeDesignSat")).Count -eq 4) "whole-design method is reported"
Check-Result (([regex]::Matches($output, "time_budget_seconds: 5")).Count -eq 4) "requested time budget is forwarded"
Check-Result (([regex]::Matches($output, "compared_output_count: 2")).Count -eq 3) "all outputs are compared for equivalent designs"
Check-Result (([regex]::Matches($output, "compared_dff_d_count: 0")).Count -eq 4) "DFF.D comparison count is always explicit"
Check-Result ($output.Contains("Interface mismatch; whole-design equivalence was not attempted for all endpoints.")) "interface mismatch is explicit"
Check-Result ($output.Contains("missing_output_names (1):`r`n  y") -or $output.Contains("missing_output_names (1):`n  y")) "renamed output reports missing original name"
Check-Result ($output.Contains("extra_output_names (1):`r`n  y_renamed") -or $output.Contains("extra_output_names (1):`n  y_renamed")) "renamed output reports extra current name"
Check-Result ($output.Contains("report_ok: false") -and $output.Contains("equivalent: false")) "interface mismatch report is non-equivalent"
Check-Result ($output.Contains("time_budget_seconds must be a finite number greater than zero.")) "invalid budget is rejected"
Check-Result (([regex]::Matches($output, "status: error")).Count -eq 2) "only missing baseline and invalid budget are errors"
Check-Result (([regex]::Matches($output, "status: timeout")).Count -eq 0) "small equivalence checks do not time out"
Check-Result (([regex]::Matches($output, "status: unsupported")).Count -eq 0) "supported circuit has no unsupported result"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 11) "every Batch 4 command returns one envelope"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 11) "every Batch 4 envelope is closed"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
