param(
    [Parameter(Mandatory = $false)]
    [string]$Executable = (Join-Path $PSScriptRoot "..\..\tools.exe")
)

$commands = @(
    "read mini test/test62/candidate_domain.v"
    "func_search pattern BUF target_pi --scope net_fanin target_pi"
    "func_search pattern BUF target_pi --scope net_fanin target_pi --include-boundary-signals"
    "func_search pattern AND target_pi --scope net_fanin target_pi --include-boundary-signals"
    "func_search pattern AND target_pi --scope net_fanin target_pi --include-boundary-signals --allow-same"
    "func_search pattern BUF target_const --scope net_fanin target_const --include-boundary-signals"
    "func_search pattern BUF target_po --scope net_fanin target_po"
    "func_search pattern BUF target_po --scope net_fanin target_po --include-boundary-signals"
    "func_search pattern BUF target_q --scope net_fanin target_q"
    "func_search pattern BUF target_q_po --scope net_fanin target_q_po"
    "func_search pattern BUF target_q_po --scope net_fanin target_q_po --include-boundary-signals"
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

Check-Result ($responses.Count -eq 12) "every candidate-domain command returns one envelope"
if ($responses.Count -eq 12) {
    $piDefault = $responses[1]
    $piBoundary = $responses[2]
    $sameDisabled = $responses[3]
    $sameEnabled = $responses[4]
    $constantScope = $responses[5]
    $poDefault = $responses[6]
    $poBoundary = $responses[7]
    $dffQDefault = $responses[8]
    $dffQPoDefault = $responses[9]
    $dffQPoBoundary = $responses[10]

    Check-Result `
        ($piDefault -match "report_status: NO_MATCH" -and
         $piDefault -match "complete: true" -and
         $piDefault -match "candidate_signal_count: 0") `
        "PI is excluded from the default scoped candidate domain"

    Check-Result `
        ($piBoundary -match "report_status: MATCH_FOUND" -and
         $piBoundary -match "candidate_signal_count: 1" -and
         $piBoundary -match "operand_1: a") `
        "boundary opt-in admits a PI operand"

    Check-Result `
        ($piBoundary -match "candidate_signal_count: 1" -and
         $piBoundary -notmatch "operand_1: target_pi") `
        "the target net remains excluded from its own candidate domain"

    Check-Result `
        ($sameDisabled -match "report_status: NO_MATCH" -and
         $sameDisabled -match "complete: true" -and
         $sameDisabled -match "candidate_signal_count: 1") `
        "binary search excludes a same-signal pair by default"

    Check-Result `
        ($sameEnabled -match "report_status: MATCH_FOUND" -and
         $sameEnabled -match "operand_1: a" -and
         $sameEnabled -match "operand_2: a") `
        "allow-same admits the functional pair (a,a)"

    Check-Result `
        ($constantScope -match "report_status: NO_MATCH" -and
         $constantScope -match "complete: true" -and
         $constantScope -match "candidate_signal_count: 0") `
        "constant literals remain excluded after boundary opt-in"

    Check-Result `
        ($poDefault -match "report_status: NO_MATCH" -and
         $poDefault -match "complete: true" -and
         $poDefault -match "candidate_signal_count: 0") `
        "non-target PO operands are excluded by default"

    Check-Result `
        ($poBoundary -match "report_status: MATCH_FOUND" -and
         $poBoundary -match "operand_1: po_source") `
        "boundary opt-in admits a non-target PO operand"

    Check-Result `
        ($dffQDefault -match "report_status: MATCH_FOUND" -and
         $dffQDefault -match "operand_1: q") `
        "current backend admits a driven DFF.Q operand by default"

    Check-Result `
        ($dffQPoDefault -match "report_status: NO_MATCH" -and
         $dffQPoDefault -match "complete: true" -and
         $dffQPoDefault -match "candidate_signal_count: 0") `
        "DFF.Q that is also a PO follows the default port exclusion policy"

    Check-Result `
        ($dffQPoBoundary -match "report_status: MATCH_FOUND" -and
         $dffQPoBoundary -match "operand_1: q_po") `
        "boundary opt-in admits DFF.Q that is also a PO"
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
