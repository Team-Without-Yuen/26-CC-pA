param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$outputDirectory = "Testing/test9"
$outputPath = "$outputDirectory/tools_session_output.v"
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$commands = @(
    "structure_query summary"
    "read mini test/test9/tools_session_circuit.v"
    "structure_query summary"
    "structure_query gate_fanout g0"
    "structure_query is_connected g0 z"
    "cone_query net_fanin y"
    "path_query exists net:a net:y"
    "depth_query global_critical"
    "func_query truth_status y"
    "read mini test/test9/does_not_exist.v"
    "structure_query summary"
    "edit_apply replace_type whole OR -allow NOR NOT"
    "write $outputPath"
    "help"
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

Check-Result ($output.Contains("NO_DESIGN_LOADED")) "query before read is rejected"
Check-Result ($output.Contains("Design loaded and original snapshot created.")) "read creates original snapshot"
Check-Result ($output.Contains("original_snapshot_available: true")) "read reports snapshot availability"
Check-Result ($output.Contains("mode: gate_fanout")) "connectivity command uses dispatcher"
Check-Result ($output.Contains("command: structure_query")) "basic and direct connectivity share one public command"
Check-Result ($output.Contains("connected: no")) "false connectivity is explicit"
Check-Result ($output.Contains("mode: net_fanin")) "cone command uses dispatcher"
Check-Result ($output.Contains("mode: exists")) "path command uses dispatcher"
Check-Result ($output.Contains("mode: global_critical")) "depth command uses dispatcher"
Check-Result ($output.Contains("status: NON_CONSTANT")) "function command uses dispatcher"
Check-Result (([regex]::Matches($output, "gates: 4")).Count -eq 2) "failed read preserves current design"
Check-Result ($output.Contains("Failed to read design: mini test/test9/does_not_exist.v")) "failed read returns structured error"
Check-Result ($output.Contains("command: edit_apply")) "edit command uses dispatcher"
Check-Result ($output.Contains("design_revision: 1")) "successful edit increments revision"
Check-Result ($output.Contains("Current design written.")) "write uses current design"
Check-Result (Test-Path -LiteralPath $outputPath) "write creates output file"
Check-Result ($output.Contains("structure_query <mode>") -and
              -not $output.Contains("basic_query <mode>") -and
              -not $output.Contains("conn_query <mode>")) "help exposes only structure_query"
Check-Result (([regex]::Matches($output, "design_revision: 0")).Count -ge 11) "read-only commands keep revision zero"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 15) "every command returns one response envelope"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 15) "every response envelope is closed"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
