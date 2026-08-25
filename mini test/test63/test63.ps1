param(
    [Parameter(Mandatory = $false)]
    [string]$Executable = (Join-Path $PSScriptRoot "..\..\tools.exe")
)

$commands = @(
    "read mini test/test63/constant_signals.v"
    "func_search constant_signals zero"
    "func_search constant_signals zero --all"
    "func_search constant_signals one --all"
    "func_search constant_signals either --all"
    "func_search constant_signals either --all --include-boundary-signals"
    "func_search constant_signals zero --all --scope net_fanin out_zero"
    "func_search constant_signals either --all --max-results 1"
    "func_search constant_signals invalid"
    "func_search constant_signals zero --allow-same"
    "func_search constant_signals zero --max-results 1"
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
$artifactPaths = @()

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

function Get-OutputFile {
    param([string]$Response)
    $match = [regex]::Match($Response, "(?m)^  output_file: (.+)$")
    if (!$match.Success) { return $null }
    return $match.Groups[1].Value.Trim()
}

function Read-Artifact {
    param([string]$Response)
    $path = Get-OutputFile $Response
    if (!$path -or !(Test-Path -LiteralPath $path)) { return "" }
    $script:artifactPaths += $path
    return Get-Content -LiteralPath $path -Raw
}

Check-Result ($responses.Count -eq 13) "every functional-constant command returns one envelope"
if ($responses.Count -eq 13) {
    $findAnyZero = $responses[1]
    $allZero = $responses[2]
    $allOne = $responses[3]
    $allEither = $responses[4]
    $allBoundary = $responses[5]
    $scopedZero = $responses[6]
    $limited = $responses[7]
    $invalidFilter = $responses[8]
    $invalidOption = $responses[9]
    $invalidLimit = $responses[10]
    $help = $responses[11]

    $zeroArtifact = Read-Artifact $allZero
    $oneArtifact = Read-Artifact $allOne
    $eitherArtifact = Read-Artifact $allEither
    $boundaryArtifact = Read-Artifact $allBoundary
    $scopeArtifact = Read-Artifact $scopedZero
    $limitedArtifact = Read-Artifact $limited

    Check-Result `
        ($findAnyZero -match "status: ok" -and
         $findAnyZero -match "report_status: MATCH_FOUND" -and
         $findAnyZero -match "search_type: FUNCTIONAL_CONSTANT_SIGNALS" -and
         $findAnyZero -match "constant_filter: ZERO" -and
         $findAnyZero -match "constant_value: 0" -and
         $findAnyZero -match "proven_constant: true" -and
         $findAnyZero -match "proof_method: AIG_(LITERAL_EQUALITY|INCREMENTAL_SAT)") `
        "FindAny returns a proven constant-zero witness"

    Check-Result `
        ($allZero -match "status: ok" -and
         $allZero -match "complete: true" -and
         $allZero -match "match_count: 2" -and
         $allZero -match "constant_zero_count: 2" -and
         $zeroArtifact -match "query_type: FUNCTIONAL_CONSTANT_SIGNALS" -and
         $zeroArtifact -match "constant_filter: ZERO" -and
         ([regex]::Matches(
             $zeroArtifact,
             "(?m)^Constant signal [0-9]+\r?$"
          ).Count -eq 2) -and
         $zeroArtifact -match "net: zero_xor" -and
         $zeroArtifact -match "net: zero_and") `
        "FindAll zero reports both internal functionally-zero nets"

    Check-Result `
        ($allOne -match "status: ok" -and
         $allOne -match "match_count: 1" -and
         $allOne -match "constant_one_count: 1" -and
         $oneArtifact -match "constant_filter: ONE" -and
         $oneArtifact -match "net: one_xnor") `
        "FindAll one reports the internal functionally-one net"

    Check-Result `
        ($allEither -match "status: ok" -and
         $allEither -match "match_count: 3" -and
         $allEither -match "constant_zero_count: 2" -and
         $allEither -match "constant_one_count: 1" -and
         $eitherArtifact -match "constant_filter: EITHER" -and
         $eitherArtifact -match "Complete: yes" -and
         $eitherArtifact -match "all_candidates_examined: true") `
        "Either combines the complete zero and one result sets"

    Check-Result `
        ($allBoundary -match "status: ok" -and
         $allBoundary -match "match_count: 6" -and
         $allBoundary -match "constant_zero_count: 4" -and
         $allBoundary -match "constant_one_count: 2" -and
         $boundaryArtifact -match "include_pi_po_port_signals: true" -and
         $boundaryArtifact -match "net: out_zero" -and
         $boundaryArtifact -match "net: out_one" -and
         $boundaryArtifact -match "net: out_zero_po") `
        "boundary opt-in includes functionally constant output ports"

    Check-Result `
        ($scopedZero -match "status: ok" -and
         $scopedZero -match "scope: NET_FANIN" -and
         $scopedZero -match "scope_name: out_zero" -and
         $scopedZero -match "match_count: 1" -and
         $scopeArtifact -match "net: zero_xor" -and
         $scopeArtifact -notmatch "net: zero_and") `
        "net_fanin scope restricts the candidate set"

    Check-Result `
        ($limited -match "status: partial" -and
         $limited -match "report_status: RESULT_LIMIT_REACHED" -and
         $limited -match "complete: false" -and
         $limited -match "truncated: true" -and
         $limited -match "match_count: 1" -and
         $limitedArtifact -match "max_results: 1" -and
         $limitedArtifact -match "Complete: no") `
        "an explicit result limit is reported as partial"

    Check-Result `
        ($invalidFilter -match "status: error" -and
         $invalidFilter -match "filter must be zero, one, or either") `
        "invalid constant filters are rejected"

    Check-Result `
        ($invalidOption -match "status: error" -and
         $invalidOption -match "--allow-same is only valid") `
        "pair-only options are rejected for constant search"

    Check-Result `
        ($invalidLimit -match "status: error" -and
         $invalidLimit -match "--max-results is only valid with --all") `
        "result limits require explicit FindAll"

    $functionSearchHelpMatch = [regex]::Match(
        $help,
        "(?s)Function search\r?\n(.*?)\r?\nSequential pattern query")
    $functionSearchHelp = if ($functionSearchHelpMatch.Success) {
        $functionSearchHelpMatch.Groups[1].Value
    } else {
        ""
    }
    Check-Result `
        ($functionSearchHelp -match "constant_signals <zero\|one\|either>" -and
         $functionSearchHelp -notmatch "--patterns" -and
         $functionSearchHelp -notmatch "--time-limit") `
        "public help documents the mode without exposing backend tuning knobs"
}

foreach ($path in ($artifactPaths | Select-Object -Unique)) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
    }
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
