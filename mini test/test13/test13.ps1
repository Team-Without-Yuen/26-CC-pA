param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test13/tools_batch5_circuit.v"
    "cone_query net_fanin y"
    "cone_query shared_fanin y z"
    "depth_query po_exceeding 2"
    "depth_query exceeding 2"
    "path_query direct_pi_po"
    "path_query direct_pi_po -req gate:g_shared"
    "path_query exists net:missing po:y"
    "path_query exists pi:a po:y -req gate:no_such_gate"
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

Check-Result ($output.Contains("mode: shared_fanin")) "shared fanin mode is public"
Check-Result ($output.Contains("second source: z")) "shared fanin reports both sources"
Check-Result ($output.Contains("Cone gates (1):") -and $output.Contains("g_shared")) "shared fanin finds only g_shared"
Check-Result ($output.Contains("AND : 1") -and $output.Contains("OR : 1") -and $output.Contains("NOT : 1")) "cone gate-type counts are reported"
Check-Result ($output.Contains("mode: po_exceeding") -and $output.Contains("Report count: 2")) "PO-only threshold excludes DFF.D"
Check-Result ($output.Contains("mode: exceeding") -and $output.Contains("Report count: 3")) "all timing endpoints include DFF.D"
Check-Result ($output.Contains("Total direct PI-to-PO connections: 1")) "depth-0 PI-to-PO connection is counted"
Check-Result ($output.Contains("Connection 1:") -and $output.Contains("Depth: 0")) "direct connection includes a depth-0 witness"
Check-Result ($output.Contains("DirectPiPoConnections does not accept endpoints or path constraints")) "direct mode rejects ignored constraints"
Check-Result ($output.Contains("Unresolved startpoints (1):") -and $output.Contains("net:missing")) "invalid startpoint is not reported as no-path"
Check-Result ($output.Contains("Unresolved required nodes (1):") -and $output.Contains("gate:no_such_gate")) "invalid path constraint is explicit"
Check-Result (([regex]::Matches($output, "status: error")).Count -eq 3) "only invalid path requests return errors"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 10) "every Batch 5 command returns one envelope"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 10) "every Batch 5 envelope is closed"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
