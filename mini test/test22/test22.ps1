param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test22/symmetry_circuit.v"
    "func_query symmetry sym_and a b"
    "func_query symmetry asym a b"
    "func_query symmetry absent a b"
    "func_query symmetry one_support a b"
    "func_query symmetry sym_bus a b"
    "func_query symmetry mixed_bus a b"
    "func_query symmetry dff_sym q a"
    "func_query symmetry bus_pair bus[0] bus[1]"
    "func_query symmetry sym_and bus b"
    "func_query symmetry sym_and internal_c b"
    "func_query symmetry sym_and a a"
    "func_query symmetry missing_target a b"
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
    ($output -match "(?s)net A: sym_and.*?status: SYMMETRIC.*?solver status: UNSAT.*?symmetric: yes") `
    "UNSAT proves ordinary scalar symmetry"
Check-Result `
    ($output -match "(?s)net A: asym.*?status: NOT_SYMMETRIC.*?solver status: SAT.*?counterexample found: yes.*?Mismatched target bits \(1\):.*?asym") `
    "SAT returns a concrete asymmetry counterexample"
Check-Result `
    ($output -match "(?s)net A: absent.*?status: SYMMETRIC.*?solver status: NOT_NEEDED.*?input A in structural support: no.*?input B in structural support: no") `
    "both absent inputs prove symmetry without running SAT"
Check-Result `
    ($output -match "(?s)net A: one_support.*?status: NOT_SYMMETRIC.*?input A in structural support: yes.*?input B in structural support: no.*?solver status: SAT") `
    "one relevant and one irrelevant input are still compared exactly"
Check-Result `
    ($output -match "(?s)net A: sym_bus.*?status: SYMMETRIC.*?solver status: UNSAT") `
    "all bits of a symmetric bus target are checked"
Check-Result `
    ($output -match "(?s)net A: mixed_bus.*?status: NOT_SYMMETRIC.*?Mismatched target bits \(1\):.*?mixed_bus\[1\]") `
    "a single asymmetric bus bit makes the whole target asymmetric"
Check-Result `
    ($output -match "(?s)net A: dff_sym.*?symmetry input A: q.*?symmetry input B: a.*?status: SYMMETRIC") `
    "DFF.Q is accepted as a pseudo-primary symmetry input"
Check-Result `
    ($output -match "(?s)net A: bus_pair.*?status: SYMMETRIC.*?symmetry input A: bus\[0\].*?symmetry input B: bus\[1\]") `
    "individual bus bits are accepted as scalar inputs"
Check-Result `
    ($output.Contains("Symmetry requires two existing scalar input nets")) `
    "a whole bus cannot be silently treated as one input"
Check-Result `
    ($output.Contains("Symmetry inputs must be primary inputs or DFF.Q pseudo-primary inputs")) `
    "internal driven nets are rejected as independent variables"
Check-Result `
    ($output.Contains("Symmetry requires two distinct input nets")) `
    "swapping an input with itself is rejected as an invalid question"
Check-Result `
    ($output.Contains("Symmetry target not found: missing_target")) `
    "missing target is an explicit error"
Check-Result `
    ($output.Contains("symmetry <target_net_or_bus> <input_a> <input_b>")) `
    "help exposes the symmetry command"
Check-Result `
    (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 15 -and `
     ([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 15) `
    "every request has one complete response envelope"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
