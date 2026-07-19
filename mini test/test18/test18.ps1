param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test18/graph_cut_circuit.v"
    "path_query pi_po_cut unique_cut"
    "path_query pi_po_cut branch_p"
    "path_query mandatory_nodes net:a net:y"
    "path_query mandatory_nodes net:branch_p net:branch_q"
    "path_query mandatory_nodes net:missing net:y"
    "path_query pi_po_cut a"
    "path_query is_separator net:a net:y join_r"
    "path_query is_separator net:branch_p net:branch_q join_r"
    "graph_query articulation_between a y"
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
    ($output -match "(?s)Status: CUT_NET.*?Candidate: unique_cut.*?Is separator: yes.*?Witness startpoint: b.*?Witness endpoint: z") `
    "unique internal chain is a PI-to-PO cut with witness pair"
Check-Result `
    ($output -match "(?s)Status: NOT_CUT_NET.*?Candidate: branch_p.*?Path exists: yes.*?Is separator: no") `
    "reconvergent branch is not a cut when an alternate branch exists"
Check-Result `
    ($output -match "(?s)Status: ARTICULATION_POINTS_FOUND.*?Path exists: yes.*?Mandatory internal nets \(1\):\s+join_r") `
    "articulation query returns only the common directed dominator"
Check-Result `
    ($output -match "(?s)Status: NO_PATH.*?Path exists: no.*?Mandatory internal nets \(0\)") `
    "no-path result is distinct from an empty connected articulation list"
Check-Result `
    ($output.Contains("net:missing")) `
    "invalid source is explicit"
Check-Result `
    ($output.Contains("PI-to-PO cut candidate must be an internal non-constant net")) `
    "PI endpoint cannot be reported as an internal cut"
Check-Result `
    ($output -match "(?s)mode: is_separator.*?Status: SEPARATOR.*?Candidate: join_r.*?Is separator: yes") `
    "generic endpoint separator mode uses the same path API"
Check-Result `
    ($output -match "(?s)mode: is_separator.*?Status: NO_PATH.*?Path exists: no.*?Is separator: no") `
    "separator query preserves no-path status"
Check-Result `
    ($output.Contains("command: graph_query") -and
     $output.Contains("Articulation net names (1):")) `
    "legacy graph_query remains executable"
Check-Result `
    ($output.Contains("mandatory_nodes") -and
     -not $output.Contains("graph_query pi_po_cut") -and
     -not $output.Contains("reg_path_query <mode>")) `
    "public help exposes only the consolidated path entry"
Check-Result `
    (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 12 -and `
     ([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 12) `
    "every path consolidation command has one response envelope"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
