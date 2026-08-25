param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$artifact = "Testing/path_literal_test35.txt"
$tiedArtifact = "Testing/path_tied_input_test35.txt"
foreach ($path in @($artifact, $tiedArtifact)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

$commands = @(
    "read mini test/test35/path_compact_circuit.v"
    "path_query enumerate net:a net:y -out $artifact -time_limit 10"
    "path_query enumerate net:a net:y -count_only -time_limit 10"
    "read mini test/test35/tied_input_path.v"
    "path_query enumerate net:a net:y -out $tiedArtifact -time_limit 10"
    "path_query enumerate net:a net:y -count_only -time_limit 10"
    "read mini test/test35/tombstone_path.v"
    "path_query exists net:dead_mid net:dead_mid"
    "edit_apply remove_dead_logic"
    "path_query exists net:dead_mid net:dead_mid"
    "path_query exists gate_out:g_dead_mid net:y"
    "path_query exists gate_in:g_dead_out:0 net:y"
    "path_query enumerate net:a net:y -req net:dead_mid -count_only"
    "path_query enumerate net:a net:y -avoid gate:g_dead_mid -count_only"
    "path_query exists net:a net:y"
    "path_query exists net:b net:y"
    "path_query exists gate_out:g_live net:y"
    "path_query exists net:a gate_in:g_output:0"
    "exit"
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

Check-Result ($responses.Count -eq 19) "every command returns one envelope"
if ($responses.Count -ge 19) {
    Check-Result `
        ($responses[1] -match "status: ok" -and
         $responses[1] -match "Total paths: 4" -and
         $responses[1] -match "Complete enumeration: yes" -and
         $responses[1] -match "Wrote paths to file: yes") `
        "full enumeration reports four complete paths"

    Check-Result `
        ($responses[2] -match "status: ok" -and
         $responses[2] -match "Total paths: 4" -and
         $responses[2] -match "Complete enumeration: yes" -and
         $responses[2] -match "Count only: yes") `
        "count-only DP agrees with full enumeration"

    Check-Result `
        ($responses[4] -match "status: ok" -and
         $responses[4] -match "Total paths: 2" -and
         $responses[4] -match "Complete enumeration: yes" -and
         $responses[4] -match "Wrote paths to file: yes") `
        "tied-input enumeration reports two unique structural paths"

    Check-Result `
        ($responses[5] -match "status: ok" -and
         $responses[5] -match "Total paths: 2" -and
         $responses[5] -match "Complete enumeration: yes" -and
         $responses[5] -match "Count only: yes") `
        "tied-input count-only DP agrees with structural enumeration"

    Check-Result `
        ($responses[7] -match "status: ok" -and
         $responses[7] -match "(?m)^Yes\r?$") `
        "active start-equals-end net remains a valid depth-zero path"

    Check-Result `
        ($responses[8] -match "status: ok" -and
         $responses[8] -match "operation_name: edit_apply:remove_dead_logic" -and
         $responses[8] -match "report_changed: true" -and
         $responses[8] -match "removed_gate_count: 2" -and
         $responses[8] -match "removed_net_count: 2") `
        "dead-logic cleanup creates tombstones"

    foreach ($case in @(
        @{ Index = 9; Name = "removed net endpoint is unresolved" },
        @{ Index = 10; Name = "removed gate output endpoint is unresolved" },
        @{ Index = 11; Name = "removed gate input endpoint is unresolved" },
        @{ Index = 12; Name = "removed required net is unresolved" },
        @{ Index = 13; Name = "removed avoided gate is unresolved" }
    )) {
        Check-Result `
            ($responses[$case.Index] -match "status: error" -and
             $responses[$case.Index] -match "unresolved") `
            $case.Name
    }

    Check-Result `
        ($responses[14] -match "status: ok" -and
         $responses[14] -match "(?m)^Yes\r?$") `
        "active path remains queryable after cleanup"

    Check-Result `
        ($responses[15] -match "status: ok" -and
         $responses[15] -match "(?m)^No\r?$") `
        "active unreachable endpoints remain a valid no-path result"

    Check-Result `
        ($responses[16] -match "status: ok" -and
         $responses[16] -match "(?m)^Yes\r?$") `
        "active gate output endpoint remains queryable after cleanup"

    Check-Result `
        ($responses[17] -match "status: ok" -and
         $responses[17] -match "(?m)^Yes\r?$") `
        "active gate input endpoint remains queryable after cleanup"
}

Check-Result (Test-Path -LiteralPath $artifact) "literal artifact is created"
if (Test-Path -LiteralPath $artifact) {
    $lines = @(Get-Content -LiteralPath $artifact)
    $text = $lines -join "`n"
    $pathLines = @($lines | Where-Object {
        $_ -match '^Path [0-9]+: '
    })

    Check-Result `
        ($text -match "Format: LITERAL_PATH_V1" -and
         $text -match "Expected paths: 4" -and
         $text -match "Written paths: 4" -and
         $text -match "Complete: yes" -and
         $text -match "Timed out: no") `
        "artifact header and footer report a complete exact result"
    Check-Result ($pathLines.Count -eq 4) "artifact contains exactly four path records"

    Check-Result `
        (($pathLines | Where-Object {
            $_ -notmatch '^Path [0-3]: a -> .+ -> y$'
        }).Count -eq 0) `
        "every literal record directly describes a path from input a to output y"
}

Check-Result (Test-Path -LiteralPath $tiedArtifact) "tied-input literal artifact is created"
if (Test-Path -LiteralPath $tiedArtifact) {
    $tiedLines = @(Get-Content -LiteralPath $tiedArtifact)
    $tiedText = $tiedLines -join "`n"
    $tiedPathLines = @($tiedLines | Where-Object {
        $_ -match '^Path [0-9]+: '
    })

    Check-Result `
        ($tiedText -match "Expected paths: 2" -and
         $tiedText -match "Written paths: 2" -and
         $tiedText -match "Complete: yes" -and
         $tiedText -match "Timed out: no") `
        "tied-input artifact header and footer report two complete paths"
    Check-Result ($tiedPathLines.Count -eq 2) `
        "tied-input artifact contains exactly two records"
    Check-Result `
        (@($tiedPathLines | Where-Object { $_ -match 'g_tied\(AND\)' }).Count -eq 1) `
        "tied-input gate contributes one structural path"
    Check-Result `
        (@($tiedPathLines | Where-Object { $_ -match 'g_distinct\(BUF\)' }).Count -eq 1) `
        "different gate instance remains a distinct path"
}

Remove-Item -LiteralPath $artifact -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $tiedArtifact -Force -ErrorAction SilentlyContinue
Write-Output "Passed: $passed"
Write-Output "Failed: $failed"
if ($failed -ne 0) {
    exit 1
}
