param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test10/tools_batch2_circuit.v"
    "cone_query largest_output"
    "depth_query gate_on_critical g0"
    "depth_query deepest_output"
    "func_query boolean_expression y"
    "func_query simplified_expression y 1"
    "func_query support_pi y"
    "path_query enumerate all_pi all_po -count_only -max_paths 20 -time_limit 55"
    "path_query enumerate all_pi all_po -count_only -max_paths 2 -time_limit 55"
    "path_query enumerate all_dff_q all_dff_d -count_only -max_paths 10 -time_limit 55"
    "quit"
) -join "`n"

$output = $commands | & $Executable 2>&1 | Out-String
$passed = 0
$failed = 0

function Check-Result {
    param(
        [bool]$Condition,
        [string]$Name
    )

    if ($Condition) {
        $script:passed++
        Write-Output "[PASS] $Name"
    } else {
        $script:failed++
        Write-Output "[FAIL] $Name"
    }
}

Check-Result ($output.Contains("mode: largest_output")) "largest output cone mode is exposed"
Check-Result ($output.Contains("checked primary outputs: 4")) "largest output scans all PO bits"
Check-Result ($output.Contains("source: y")) "largest output cone selects y"
Check-Result ($output.Contains("mode: gate_on_critical")) "gate-on-critical mode is exposed"
Check-Result ($output.Contains("Gate on critical path: yes")) "g0 lies on a maximum-depth path"
Check-Result ($output.Contains("mode: deepest_output")) "deepest output mode is exposed"
Check-Result ($output.Contains("Worst endpoint: y depth=3")) "deepest output is y at depth 3"
Check-Result ($output.Contains("expression: NOT(OR(AND(a, b), c))")) "full Boolean expression is returned"
Check-Result ($output.Contains("expression: NOT(n2)")) "depth-limited expression is returned"
Check-Result ($output.Contains("expression depth limited: yes")) "depth-limited flag is returned"
Check-Result ($output.Contains("Support primary inputs (3):")) "support PI list is returned"
Check-Result ($output.Contains("Total paths: 4")) "all_pi/all_po count-only enumeration is complete"
Check-Result (([regex]::Matches($output, "Total paths: 4")).Count -ge 2) "legacy max_paths does not truncate complete enumeration"
Check-Result (([regex]::Matches($output, "Path limit reached: no")).Count -eq 3) "path-limit status remains false for complete enumeration"
Check-Result (([regex]::Matches($output, "Complete enumeration: yes")).Count -eq 3) "all count-only enumerations report complete results"
Check-Result (([regex]::Matches($output, "Total paths: 1")).Count -ge 1) "register paths use PathQuery DFF endpoint presets"
Check-Result (([regex]::Matches($output, "Count only: yes")).Count -eq 3) "count-only state is printed for all enumerate calls"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 11) "every Batch 2 command returns one envelope"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 11) "every Batch 2 envelope is closed"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
