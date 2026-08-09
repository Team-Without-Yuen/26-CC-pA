param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$smallOut = "Testing/path_pruning_small_tmp.txt"
$largeOut = "Testing/path_pruning_test14_tmp.txt"
$timeoutOut = "Testing/path_pruning_test37_timeout_tmp.txt"

Remove-Item -LiteralPath $smallOut, $largeOut, $timeoutOut -Force -ErrorAction SilentlyContinue

$commands = @(
    "read mini test/test33/path_pruning_circuit.v"
    "path_query enumerate net:a net:y -count_only -time_limit 5"
    "path_query enumerate net:a net:y -req gate:g2 -count_only -time_limit 5"
    "path_query enumerate net:a net:y -avoid net:n1 -count_only -time_limit 5"
    "path_query enumerate net:a net:y -out $smallOut -max_print 0 -time_limit 5"
    "read NewTestCase/test14/test14.v"
    "path_query enumerate net:n0[0] net:n63[1] -count_only -time_limit 10"
    "path_query enumerate net:n0[0] net:n63[1] -out $largeOut -max_print 0 -time_limit 10"
    "read NewTestCase/test37/test37.v"
    "path_query enumerate all_dff_q all_dff_d -count_only -time_limit 10"
    "path_query enumerate all_dff_q all_dff_d -out $timeoutOut -max_print 0 -time_limit 0.05"
    "read NewTestCase/test40/test40.v"
    "path_query max_depth all_dff_q all_dff_d"
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

function First-Line {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) {
        return Get-Content -Path $Path -TotalCount 1
    }
    return ""
}

Check-Result ($responses.Count -eq 14) "every command returns one tool envelope"

if ($responses.Count -ge 13) {
    Check-Result `
        ($responses[1] -match "status: ok" -and
         $responses[1] -match "Total paths: 2" -and
         $responses[1] -match "Complete enumeration: yes") `
        "small circuit count_only preserves the exact two a-to-y paths"

    Check-Result `
        ($responses[2] -match "status: ok" -and
         $responses[2] -match "Total paths: 1" -and
         $responses[2] -match "Complete enumeration: yes") `
        "required gate filtering keeps only the g2 path"

    Check-Result `
        ($responses[3] -match "status: ok" -and
         $responses[3] -match "Total paths: 1" -and
         $responses[3] -match "Complete enumeration: yes") `
        "avoid net filtering removes the n1 branch"

    Check-Result `
        ($responses[4] -match "status: ok" -and
         $responses[4] -match "Total paths: 2" -and
         $responses[4] -match "Wrote paths to file: yes" -and
         (First-Line $smallOut) -eq "Total paths: 2" -and
         (Get-Content -LiteralPath $smallOut -Raw) -match "Format: COMPACT_PATH_V3" -and
         (Get-Content -LiteralPath $smallOut -Raw) -match "Written paths: 2") `
        "small streaming output writes the compact exact-count artifact"

    Check-Result `
        ($responses[6] -match "status: ok" -and
         $responses[6] -match "Total paths: 289366" -and
         $responses[6] -match "Complete enumeration: yes" -and
         $responses[6] -match "Timed out: no") `
        "released test14 count_only completes with the known path count"

    Check-Result `
        ($responses[7] -match "status: ok" -and
         $responses[7] -match "Total paths: 289366" -and
         $responses[7] -match "Wrote paths to file: yes" -and
         (First-Line $largeOut) -eq "Total paths: 289366" -and
         (Get-Content -LiteralPath $largeOut -Raw) -match "Written paths: 289366" -and
         (Get-Content -LiteralPath $largeOut -Raw) -match "Complete: yes") `
        "released test14 streaming output completes in compact format"

    Check-Result `
        ($responses[9] -match "status: ok" -and
         $responses[9] -match "Total paths: 16548172" -and
         $responses[9] -match "Complete enumeration: yes" -and
         $responses[9] -match "Timed out: no") `
        "released test37 all-register count_only uses DP and completes within the time budget"

    Check-Result `
        ($responses[10] -match "status: timeout" -and
         $responses[10] -match "Complete enumeration: no" -and
         $responses[10] -match "Timed out: yes" -and
         $responses[10] -match "Wrote paths to file: yes") `
        "released test37 streaming timeout returns an explicit partial envelope"

    Check-Result `
        ($responses[12] -match "status: ok" -and
         $responses[12] -match "complete: true" -and
         $responses[12] -match "Depth: 104") `
        "released test40 all-register max depth uses multi-endpoint DP"
}

Remove-Item -LiteralPath $smallOut, $largeOut, $timeoutOut -Force -ErrorAction SilentlyContinue

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
