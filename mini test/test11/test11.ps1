param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$circuit = "mini test/test11/tools_edit_batch3_circuit.v"
$cases = @(
    @{ Command = "edit_apply rename_gate g0 g0_renamed"; Operation = "rename_gate" },
    @{ Command = "edit_apply rename_net fan fan_renamed"; Operation = "rename_net" },
    @{ Command = "edit_apply cleanup_buffers"; Operation = "cleanup_buffers" },
    @{ Command = "edit_apply collapse_double_inverter"; Operation = "collapse_double_inverter" },
    @{ Command = "edit_apply local_simplification_fixpoint"; Operation = "local_simplification_fixpoint" },
    @{ Command = "edit_apply safe_cleanup_fixpoint"; Operation = "safe_cleanup_fixpoint" },
    @{ Command = "edit_apply trim_dead_logic"; Operation = "trim_dead_logic" },
    @{ Command = "edit_apply remove_dangling_logic"; Operation = "remove_dangling_logic" },
    @{ Command = "edit_apply remove_unused_nets"; Operation = "remove_unused_nets" },
    @{ Command = "edit_apply remove_net_if_unused dead2"; Operation = "remove_net_if_unused" },
    @{ Command = "edit_apply merge_structurally_equivalent_gates"; Operation = "merge_structurally_equivalent_gates" },
    @{ Command = "edit_apply simplify_constants"; Operation = "simplify_constants" },
    @{ Command = "edit_apply simplify_same_input"; Operation = "simplify_same_input" },
    @{ Command = "edit_apply insert_buffers_for_fanout 3"; Operation = "insert_buffers_for_fanout" },
    @{ Command = "edit_apply insert_buffers_for_net fan 3"; Operation = "insert_buffers_for_specific_net" },
    @{ Command = "edit_apply insert_buffers_for_dff_control 2 -clock -reset"; Operation = "insert_buffers_for_dff_control" },
    @{ Command = "edit_apply insert_buffers_on_each_load fan"; Operation = "insert_buffers_on_each_load" },
    @{ Command = "edit_apply insert_buffer_at_driver fan"; Operation = "insert_buffer_at_driver" },
    @{ Command = "edit_apply insert_buffer_before_gate fan g0"; Operation = "insert_buffer_before_gate" },
    @{ Command = "edit_apply insert_buffers_by_gate_type AND -both"; Operation = "insert_buffers_by_gate_type" },
    @{ Command = "edit_apply convert_basis whole -allow AND NOT"; Operation = "convert_basis" },
    @{ Command = "edit_apply replace_type whole XOR -allow NAND"; Operation = "replace_type" }
)

$commands = [System.Collections.Generic.List[string]]::new()
$commands.Add("read $circuit")
$commands.Add("report_query last_edit")
foreach ($case in $cases) {
    $commands.Add("read $circuit")
    $commands.Add($case.Command)
    if ($case.Operation -eq "rename_gate") {
        $commands.Add("report_query last_edit")
    }
}
$commands.Add("read $circuit")
$commands.Add("edit_apply merge_equivalent_gates")
$commands.Add("help")
$commands.Add("quit")

$output = ($commands -join "`n") | & $Executable 2>&1 | Out-String
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

foreach ($case in $cases) {
    Check-Result `
        ($output.Contains("operation_name: edit_apply:$($case.Operation)")) `
        "public edit routes $($case.Operation)"
}

Check-Result ($output.Contains("NO_LAST_EDIT_REPORT")) "report_query rejects missing report"
Check-Result ($output.Contains("command: report_query")) "report_query last_edit is routed"
Check-Result ($output.Contains("message: Cached last edit report.")) "last edit report is cached"
Check-Result ($output.Contains("before_stats:")) "report contains before stats"
Check-Result ($output.Contains("after_stats:")) "report contains after stats"
Check-Result ($output.Contains("diff:")) "report contains diff"
Check-Result ($output.Contains("validation:")) "report contains validation"
Check-Result ($output.Contains("fanout_change:")) "report contains fanout change"
Check-Result ($output.Contains("mapping_delta:")) "report contains mapping delta"
Check-Result ($output.Contains("changed_gate_ids")) "report contains changed IDs"
Check-Result ($output.Contains("changed_gate_names")) "report contains changed names"
Check-Result (-not $output.Contains("Unknown or non-public edit_apply mode")) "all requested edits are public"
Check-Result `
    ($output.Contains("legacy implementation only proves structural identity")) `
    "ambiguous legacy merge command is rejected with an explicit replacement"
Check-Result `
    (-not $output.Contains("operation_name: edit_apply:merge_equivalent_gates")) `
    "legacy merge command cannot mutate the design through tools CLI"
$helpResponse = [regex]::Match($output, "(?s)command: help.*?TOOL_RESULT_END").Value
Check-Result `
    ($helpResponse.Contains("edit_apply merge_structurally_equivalent_gates")) `
    "help exposes the unambiguous structural merge command"
Check-Result `
    (-not $helpResponse.Contains("edit_apply merge_equivalent_gates")) `
    "help hides the ambiguous legacy merge command"
Check-Result (-not $output.Contains("equivalence_method: NotChecked")) "successful public edits have certificates"
Check-Result (([regex]::Matches($output, "report_success: true")).Count -eq ($cases.Count + 1)) "all edits succeed and cached report repeats once"
Check-Result (([regex]::Matches($output, "status: error")).Count -eq 2) "only missing report and hidden legacy command return errors"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq (2 * $cases.Count + 7)) "every command returns one envelope"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq (2 * $cases.Count + 7)) "every envelope is closed"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
