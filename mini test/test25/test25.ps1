param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test25/conditional_equivalence_circuit.v"
    "func_query conditional_equivalence y_mux y_a en 0"
    "func_query conditional_equivalence y_mux y_b en 1"
    "func_query conditional_equivalence y_mux y_a en 1"
    "func_query conditional_equivalence y_mux y_a en 2"
    "func_query conditional_equivalence y_mux y_a missing 0"
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

Check-Result ($responses.Count -eq 8) "every conditional-equivalence command returns one envelope"
if ($responses.Count -ge 8) {
    $whenZero = $responses[1]
    $whenOne = $responses[2]
    $notEquivalent = $responses[3]
    $badValue = $responses[4]
    $missingCondition = $responses[5]
    $help = $responses[6]

    Check-Result `
        ($whenZero -match "status: ok" -and
         $whenZero -match "complete: true" -and
         $whenZero -match "condition net: en" -and
         $whenZero -match "condition value: 0" -and
         $whenZero -match "equivalent: yes") `
        "en=0 proves y_mux equivalent to y_a"

    Check-Result `
        ($whenOne -match "status: ok" -and
         $whenOne -match "complete: true" -and
         $whenOne -match "condition value: 1" -and
         $whenOne -match "equivalent: yes") `
        "en=1 proves y_mux equivalent to y_b"

    Check-Result `
        ($notEquivalent -match "status: ok" -and
         $notEquivalent -match "complete: true" -and
         $notEquivalent -match "equivalent: no") `
        "a false conditional-equivalence result remains complete"

    Check-Result `
        ($badValue -match "status: error" -and
         $badValue -match "conditional_equivalence requires condition value 0 or 1" -and
         $badValue -match "complete: false") `
        "invalid condition values are rejected by the strict parser"

    Check-Result `
        ($missingCondition -match "status: error" -and
         $missingCondition -match "existing scalar condition net" -and
         $missingCondition -match "complete: false") `
        "missing condition nets are rejected"

    Check-Result `
        ($help -match "conditional_equivalence <net_a> <net_b> <condition_net> <0\|1>") `
        "public help exposes conditional-equivalence grammar"
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
