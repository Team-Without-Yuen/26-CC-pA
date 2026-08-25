param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$circuit = "mini test/test14/constant_simplification_circuit.v"
$commands = @(
    "read $circuit"
    "basic_query const_input_gates AND 0"
    "read $circuit"
    "edit_apply simplify_constants AND 0"
    "equiv_query original 5"
    "read $circuit"
    "edit_apply simplify_constants OR 1"
    "equiv_query original 5"
    "read $circuit"
    "edit_apply simplify_constants NAND 1 --inputs 2"
    "equiv_query original 5"
    "read $circuit"
    "edit_apply simplify_constants NAND 1 --inputs 3"
    "basic_query gate_info g_nand3"
    "equiv_query original 5"
    "read $circuit"
    "edit_apply simplify_constants XOR 1 --inputs 3"
    "basic_query gate_info g_xor3"
    "equiv_query original 5"
    "read $circuit"
    "edit_apply simplify_constants"
    "equiv_query original 5"
    "read mini test/test14/baseline_violation_circuit.v"
    "edit_apply simplify_constants AND 0"
    "read $circuit"
    "edit_apply simplify_constants DFF 1"
    "edit_apply simplify_constants OR 2"
    "edit_apply simplify_constants NAND 1 --inputs 0"
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
    ($output.Contains("mode: const_input_gates") -and $output.Contains("gates: 1")) `
    "constant-input query finds only AND gates tied to 0"
Check-Result `
    ($output.Contains("target_gate_type: AND") -and $output.Contains("target_const_value: 0")) `
    "report records gate-type and constant filters"
Check-Result `
    ($output.Contains("candidate_gate_names (1):") -and $output.Contains("g_and0")) `
    "AND-0 pass records the exact candidate"
Check-Result `
    ($output.Contains("target_gate_type: OR") -and $output.Contains("candidate_count: 2") -and $output.Contains("eliminated_target_gate_count: 2")) `
    "OR-1 pass simplifies internal and direct-PO drivers"
Check-Result `
    ($output.Contains("target_input_count: 2") -and $output.Contains("g_nand1")) `
    "two-input NAND filter selects g_nand1"
Check-Result `
    ($output.Contains("target_input_count: 3") -and $output.Contains("simplified_count: 1") -and $output.Contains("eliminated_target_gate_count: 0")) `
    "three-input NAND removes the neutral input without removing the NAND"
Check-Result `
    ($output.Contains("object: g_nand3") -and $output.Contains("type: NAND") -and $output.Contains("inputs=[IN1=a(PI), IN2=b(PI)]")) `
    "three-input NAND is reduced to two non-constant inputs"
Check-Result `
    ($output.Contains("object: g_xor3") -and $output.Contains("type: XNOR") -and $output.Contains("inputs=[IN1=a(PI), IN2=b(PI)]")) `
    "multi-input XOR constant parity flips XOR to XNOR"
Check-Result `
    ($output.Contains("target_gate_type: ALL") -and $output.Contains("skipped_count: 0")) `
    "global pass excludes constant DFF control pins from edit candidates"
Check-Result `
    (([regex]::Matches($output, "equivalence_method: LocalRewriteRule")).Count -eq 7) `
    "all successful constant rewrites carry local equivalence certificates"
Check-Result `
    (([regex]::Matches($output, "(?m)^  equivalent: true\r?$" )).Count -eq 6) `
    "all six rewrite variants pass whole-design SAT equivalence"
Check-Result `
    ($output.Contains("problem_a_constraints_baseline_valid: false") -and `
     $output.Contains("problem_a_constraints_valid: false") -and `
     $output.Contains("problem_a_constraints_regressed: false")) `
    "pre-existing baseline violations do not reject an unrelated valid edit"
Check-Result `
    ($output.Contains("valid combinational <gate_type> or all")) `
    "DFF filter is rejected"
Check-Result `
    ($output.Contains("Constant filter must be 0, 1, or any")) `
    "invalid constant value is rejected"
Check-Result `
    ($output.Contains("--inputs requires a positive integer")) `
    "invalid input-count filter is rejected"
Check-Result `
    (([regex]::Matches($output, "status: error")).Count -eq 3) `
    "only the three invalid requests fail"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
