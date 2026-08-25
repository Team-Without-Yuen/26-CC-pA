param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test32/opt_cli_circuit.v"
    "help"
    "opt_query critical_path_depth"
    "depth_query global_critical"
    "opt_apply critical_path_depth --scope whole --objective global --time-limit 30"
    "report_query last_edit"
    "depth_query global_critical"
    "equiv_query previous_edit 30"
    "read mini test/test32/opt_cli_circuit.v"
    "opt_apply critical_path_depth --scope gate_fanin ff0 --objective global --allowed NOR,NOT --time-limit 30"
    "report_query last_edit"
    "depth_query global_critical"
    "equiv_query previous_edit 30"
    "read mini test/test32/opt_cli_circuit.v"
    "opt_apply critical_path_depth --scope gate_fanin ff0 --objective cone --allowed NOR NOT --target-depth 0 --time-limit 30"
    "depth_query global_critical"
    "report_query last_edit"
    "opt_apply critical_path_depth --scope net_fanout q --objective cone"
    "opt_apply critical_path_depth --scope whole --objective global --time-limit 0.000001"
    "depth_query global_critical"
    "report_query last_edit"
    "read mini test/test32/scoped_optimal_circuit.v"
    "depth_query net q"
    "opt_apply critical_path_depth --scope net_fanin q --objective cone --allowed NAND NOT --time-limit 30"
    "depth_query net and_y"
    "opt_apply critical_path_depth --scope net_fanin and_y --objective cone --allowed NAND NOT --time-limit 30"
    "report_query last_edit"
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

Check-Result ($responses.Count -eq 28) "every optimization CLI command returns one envelope"
if ($responses.Count -ge 28) {
    $help = $responses[1]
    $query = $responses[2]
    $beforeDepth = $responses[3]
    $globalAccepted = $responses[4]
    $cachedGlobal = $responses[5]
    $globalDepth = $responses[6]
    $globalEquivalence = $responses[7]
    $reloadedForConstraint = $responses[8]
    $constrainedAccepted = $responses[9]
    $cachedConstrained = $responses[10]
    $constrainedDepth = $responses[11]
    $constrainedEquivalence = $responses[12]
    $reloadedForReject = $responses[13]
    $rejected = $responses[14]
    $depthAfterReject = $responses[15]
    $cachedRejected = $responses[16]
    $invalidScope = $responses[17]
    $timedOut = $responses[18]
    $depthAfterTimeout = $responses[19]
    $cachedTimeout = $responses[20]
    $lowerBoundRead = $responses[21]
    $dffDepth = $responses[22]
    $dffOptimal = $responses[23]
    $andDepth = $responses[24]
    $andOptimal = $responses[25]
    $cachedOptimal = $responses[26]

    Check-Result `
        ($help -match "opt_query critical_path_depth" -and
         $help -match "opt_apply <critical_path_depth" -and
         $help -match "gate_count_minimization" -and
         $help -match "--target-cost N" -and
         $help -match "aliases kept for compatibility") `
        "help exposes the public depth optimization contract"

    Check-Result `
        ($query -match "status: ok" -and
         $query -match "pass_kind: critical_path_depth" -and
         $query -match "candidate_count: 1" -and
         $query -match "requires_equivalence_check: false") `
        "opt_query exposes the safe pass-level candidate"

    Check-Result `
        ($beforeDepth -match "Worst endpoint: ff0.D depth=7") `
        "baseline global depth is seven"

    Check-Result `
        ($globalAccepted -match "status: ok" -and
         $globalAccepted -match "design_revision: 1" -and
         $globalAccepted -match "before_value: 7" -and
         $globalAccepted -match "after_value: 3" -and
         $globalAccepted -match "improved: true" -and
         $globalAccepted -match "candidate_accepted: true" -and
         $globalAccepted -match "whole_design_equivalence_checked: false" -and
         $globalAccepted -match "equivalence_method: CertifiedRewrite" -and
         $globalAccepted -match "changed_gate_ids \([1-9][0-9]*\):" -and
         $globalAccepted -match "changed_net_ids \([1-9][0-9]*\):") `
        "optimization accepts a certified depth improvement without whole-design SAT"

    Check-Result `
        ($cachedGlobal -match "operation_kind: DepthOptimization" -and
         $cachedGlobal -match "before_value: 7" -and
         $cachedGlobal -match "after_value: 3") `
        "report_query caches the global optimization result"

    Check-Result `
        ($globalDepth -match "Worst endpoint: ff0.D depth=3") `
        "accepted global optimization changes the current design depth"

    Check-Result `
        ($globalEquivalence -match "status: ok" -and
         $globalEquivalence -match "equivalent: true" -and
         $globalEquivalence -match "method: WholeDesignSat") `
        "global optimization preserves the previous-edit baseline"

    Check-Result `
        ($reloadedForConstraint -match "design_revision: 0" -and
         $reloadedForConstraint -match "original_snapshot_available: true") `
        "read resets state before the constrained flow"

    Check-Result `
        ($constrainedAccepted -match "status: ok" -and
         $constrainedAccepted -match "design_revision: 1" -and
         $constrainedAccepted -match "report_success: true" -and
         $constrainedAccepted -match "before_value: 7" -and
         $constrainedAccepted -match "after_value: 9" -and
         $constrainedAccepted -match "improved: false" -and
         $constrainedAccepted -match "baseline_constraints_satisfied: false" -and
         $constrainedAccepted -match "final_constraints_satisfied: true" -and
         $constrainedAccepted -match "candidate_accepted: true" -and
         $constrainedAccepted -match "whole_design_equivalence_checked: false" -and
         $constrainedAccepted -match "equivalence_method: CertifiedRewrite" -and
         $constrainedAccepted -match "allowed_gate_types \(2\):(?s).*NOR(?s).*NOT" -and
         $constrainedAccepted -match "original design violated a hard gate constraint") `
        "DFF gate_fanin NOR/NOT flow reports the hard-constraint tradeoff without exposing legacy resolution detail"

    Check-Result `
        ($cachedConstrained -match "operation_kind: DepthOptimization" -and
         $cachedConstrained -match "candidate_accepted: true" -and
         $cachedConstrained -match "whole_design_equivalence_checked: false" -and
         $cachedConstrained -match "equivalence_method: CertifiedRewrite") `
        "report_query returns the cached optimization report"

    Check-Result `
        ($constrainedDepth -match "Worst endpoint: ff0.D depth=9") `
        "current design matches the accepted constraint-compliant report"

    Check-Result `
        ($constrainedEquivalence -match "status: ok" -and
         $constrainedEquivalence -match "equivalent: true" -and
         $constrainedEquivalence -match "method: WholeDesignSat" -and
         $constrainedEquivalence -match "complete: true") `
        "previous-edit baseline remains available after opt_apply"

    Check-Result `
        ($reloadedForReject -match "design_revision: 0" -and
         $reloadedForReject -match "original_snapshot_available: true") `
        "read resets optimization state and revision"

    Check-Result `
        ($rejected -match "status: error" -and
         $rejected -match "design_revision: 0" -and
         $rejected -match "report_success: false" -and
         $rejected -match "rolled_back: true" -and
         $rejected -match "target_value: 0" -and
         $rejected -match "meets_target: false" -and
         $rejected -match "candidate_accepted: false") `
        "unmet target depth rejects and rolls back the candidate"

    Check-Result `
        ($depthAfterReject -match "Worst endpoint: ff0.D depth=7") `
        "failed optimization leaves the current design unchanged"

    Check-Result `
        ($cachedRejected -match "report_success: false" -and
         $cachedRejected -match "rolled_back: true" -and
         $cachedRejected -match "target_value: 0") `
        "failed optimization report is also cached for follow-up questions"

    Check-Result `
        ($invalidScope -match "status: error" -and
         $invalidScope -match "A cone-scoped cost function requires net_fanin or gate_fanin scope") `
        "parser rejects a cone objective with fanout scope"

    Check-Result `
        ($timedOut -match "status: timeout" -and
         $timedOut -match "design_revision: 0" -and
         $timedOut -match "report_success: false" -and
         $timedOut -match "rolled_back: false" -and
         $timedOut -match "core_status: TIMEOUT" -and
         $timedOut -match "candidate_generated: false" -and
         $timedOut -match "whole_design_equivalence_checked: false" -and
         $timedOut -match "whole_design_timed_out: false" -and
         $timedOut -match "candidate_accepted: false") `
        "exhausted pre-core time budget is explicit and never starts a candidate"

    Check-Result `
        ($depthAfterTimeout -match "Worst endpoint: ff0.D depth=7") `
        "timeout leaves the current design unchanged"

    Check-Result `
        ($cachedTimeout -match "report_success: false" -and
         $cachedTimeout -match "core_status: TIMEOUT" -and
         $cachedTimeout -match "candidate_generated: false" -and
         $cachedTimeout -match "whole_design_timed_out: false") `
        "timeout report remains available to follow-up queries"

    Check-Result `
        ($lowerBoundRead -match "status: ok" -and
         $lowerBoundRead -match "design_revision: 0") `
        "lower-bound regression circuit loads with fresh state"

    Check-Result `
        ($dffDepth -match "Worst endpoint: q depth=0") `
        "DFF.Q is a depth-zero combinational boundary"

    Check-Result `
        ($dffOptimal -match "status: no_change" -and
         $dffOptimal -match "report_success: true" -and
         $dffOptimal -match "before_value: 0" -and
         $dffOptimal -match "after_value: 0" -and
         $dffOptimal -match "core_status: NO_IMPROVEMENT" -and
         $dffOptimal -match "resolved optimization scope is empty" -and
         $dffOptimal -match "equivalence_method: StructuralIdentity" -and
         $dffOptimal -match "complete: true") `
        "DFF.Q net_fanin boundary depth zero returns original without invoking the optimizer"

    Check-Result `
        ($andDepth -match "Worst endpoint: and_y depth=2") `
        "NAND followed by NOT implements the two-input AND at depth two"

    Check-Result `
        ($andOptimal -match "status: no_change" -and
         $andOptimal -match "report_success: true" -and
         $andOptimal -match "before_value: 2" -and
         $andOptimal -match "after_value: 2" -and
         $andOptimal -match "depth 2 is optimal" -and
         $andOptimal -match "final_constraints_satisfied: true" -and
         $andOptimal -match "equivalence_method: StructuralIdentity" -and
         $andOptimal -match "complete: true") `
        "NAND/NOT two-input AND lower bound safely retains the original"

    Check-Result `
        ($cachedOptimal -match "status: ok" -and
         $cachedOptimal -match "report_success: true" -and
         $cachedOptimal -match "core_status: NO_IMPROVEMENT" -and
         $cachedOptimal -match "candidate_generated: false") `
        "proven-optimal no-change report is cached for follow-up questions"
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
