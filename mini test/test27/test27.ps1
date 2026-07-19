param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test27/test38_edit_sequence_circuit.v"
    "edit_apply insert_buffers_for_net reset 4"
    "structure_query fanout_load reset"
    "structure_query fanout_load hot"
    "edit_apply collapse_double_inverter"
    "structure_query fanout_load reset"
    "edit_apply merge_structurally_equivalent_gates"
    "equiv_query original"
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

Check-Result ($responses.Count -eq 9) "every test38-style edit command returns one envelope"
if ($responses.Count -ge 9) {
    $insert = $responses[1]
    $resetAfterInsert = $responses[2]
    $unrelatedHot = $responses[3]
    $collapse = $responses[4]
    $resetAfterCollapse = $responses[5]
    $merge = $responses[6]
    $equivalence = $responses[7]

    Check-Result `
        ($insert -match "status: ok" -and
         $insert -match "before_max_fanout: 8" -and
         $insert -match "after_max_fanout: 4" -and
         $insert -match "target_fanout: 4" -and
         $insert -match "meets_constraint: true" -and
         $insert -match "violating_net_names \(0\)") `
        "specific-net report measures only the selected buffer tree"

    Check-Result `
        ($resetAfterInsert -match "net: reset" -and
         $resetAfterInsert -match "count: 4") `
        "reset fanout reaches the requested limit"

    Check-Result `
        ($unrelatedHot -match "net: hot" -and
         $unrelatedHot -match "count: 12") `
        "an unrelated high-fanout net remains above the selected limit"

    Check-Result `
        ($collapse -match "status: ok" -and
         $collapse -match "active_gate_count_delta: -2" -and
         $collapse -match "NOT : -2" -and
         $collapse -match "BUF : 0" -and
         $collapse -match "equivalence_method: LocalRewriteRule") `
        "double-inverter collapse removes only the two NOT gates"

    Check-Result `
        ($resetAfterCollapse -match "net: reset" -and
         $resetAfterCollapse -match "count: 4") `
        "double-inverter collapse preserves the reset buffer tree"

    Check-Result `
        ($merge -match "status: ok" -and
         $merge -match "active_gate_count_delta: -2" -and
         $merge -match "NOT : -2" -and
         $merge -match "equivalence_method: StructuralIdentity") `
        "structural redundancy pass removes both intentional duplicates"

    Check-Result `
        ($equivalence -match "status: ok" -and
         $equivalence -match "equivalent: true" -and
         $equivalence -match "method: WholeDesignSat" -and
         $equivalence -match "complete: true") `
        "the cumulative test38-style edit sequence remains equivalent"
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
