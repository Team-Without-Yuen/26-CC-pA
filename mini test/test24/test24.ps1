param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test24/function_search_cli_circuit.v"
    "func_search nand_pair target"
    "func_search nand_pair target --all --max-results 64 --patterns 128 --time-limit 5"
    "func_search nand_pair target --all --max-results 1 --time-limit 5"
    "func_search nand_pair no_match --time-limit 5"
    "func_search nand_pair missing_net"
    "func_search nand_pair target --patterns 0"
    "func_search nand_pair target --time-limit 0.000000000001"
    "help"
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

Check-Result ($responses.Count -eq 10) "every Function Search command returns one envelope"
if ($responses.Count -ge 10) {
    $findAny = $responses[1]
    $findAll = $responses[2]
    $limited = $responses[3]
    $noMatch = $responses[4]
    $missing = $responses[5]
    $invalid = $responses[6]
    $timeout = $responses[7]
    $help = $responses[8]

    Check-Result `
        ($findAny -match "status: ok" -and
         $findAny -match "report_status: MATCH_FOUND" -and
         $findAny -match "found: true" -and
         $findAny -match "complete: true" -and
         $findAny -match "proof_method: SAT_UNSAT_MITER" -and
         $findAny -match "solver_status: UNSAT") `
        "FindAny returns a complete SAT-proven witness"

    Check-Result `
        ($findAll -match "status: ok" -and
         $findAll -match "report_status: MATCHES_FOUND" -and
         $findAll -match "all_candidates_examined: true" -and
         $findAll -match "truncated: false") `
        "FindAll reports a complete candidate search"

    Check-Result `
        ($limited -match "status: partial" -and
         $limited -match "complete: false" -and
         $limited -match "report_status: RESULT_LIMIT_REACHED" -and
         $limited -match "truncated: true" -and
         $limited -match "match_count: 1") `
        "result limit is reported as partial instead of complete"

    Check-Result `
        ($noMatch -match "status: ok" -and
         $noMatch -match "report_status: NO_MATCH" -and
         $noMatch -match "found: false" -and
         $noMatch -match "complete: true" -and
         $noMatch -match "all_candidates_examined: true") `
        "exhaustive no-match is distinct from unknown"

    Check-Result `
        ($missing -match "status: error" -and
         $missing -match "report_status: TARGET_NOT_FOUND" -and
         $missing -match "complete: false") `
        "missing target is an explicit request error"

    Check-Result `
        ($invalid -match "status: error" -and
         $invalid -match "--patterns requires an integer in the range 1..4096") `
        "invalid CLI limits are rejected before analysis"

    Check-Result `
        ($timeout -match "status: timeout" -and
         $timeout -match "report_status: TIMEOUT" -and
         $timeout -match "timed_out: true" -and
         $timeout -match "complete: false") `
        "time limit returns timeout instead of no-match"

    Check-Result `
        ($help -match "func_search nand_pair <target_net>" -and
         $help -match "--all" -and
         $help -match "--time-limit") `
        "public help exposes Function Search grammar"
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
