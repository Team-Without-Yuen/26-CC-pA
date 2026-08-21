param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$artifact = "Testing/path_literal_test35.txt"
if (Test-Path -LiteralPath $artifact) {
    Remove-Item -LiteralPath $artifact -Force
}

$commands = @(
    "read mini test/test35/path_compact_circuit.v"
    "path_query enumerate net:a net:y -out $artifact -time_limit 10"
    "path_query enumerate net:a net:y -count_only -time_limit 10"
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

Check-Result ($responses.Count -eq 4) "every command returns one envelope"
if ($responses.Count -ge 4) {
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

Remove-Item -LiteralPath $artifact -Force -ErrorAction SilentlyContinue
Write-Output "Passed: $passed"
Write-Output "Failed: $failed"
if ($failed -ne 0) {
    exit 1
}
