param(
    [Parameter(Mandatory = $false)]
    [string]$Executable = (Join-Path $PSScriptRoot "..\..\tools.exe")
)

$commands = @(
    "read mini test/test64/complementary_pairs.v"
    "func_search complementary_pairs signals whole"
    "func_search complementary_pairs signals whole --all"
    "func_search complementary_pairs gates whole --all"
    "func_search complementary_pairs gates whole --all --gate-type NAND"
    "func_search complementary_pairs signals net_fanin out_and --all"
    "func_search complementary_pairs signals whole --all --include-boundary-signals"
    "func_search complementary_pairs signals whole --all --max-results 1"
    "func_search complementary_pairs signals whole --gate-type NAND"
    "func_search complementary_pairs gates whole --include-boundary-signals"
    "func_search complementary_pairs signals whole --max-results 1"
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

function Read-Artifact {
    param([string]$Response)
    $match = [regex]::Match($Response, "(?m)^  output_file: (.+)$")
    if (!$match.Success) { return "" }
    $path = $match.Groups[1].Value.Trim()
    if (!(Test-Path -LiteralPath $path)) { return "" }
    $script:artifactPaths += $path
    return Get-Content -LiteralPath $path -Raw
}

Check-Result ($responses.Count -eq 13) "every complementary-pair command returns one envelope"
if ($responses.Count -eq 13) {
    $findAny = $responses[1]
    $allSignals = $responses[2]
    $allGates = $responses[3]
    $nandGates = $responses[4]
    $scoped = $responses[5]
    $boundary = $responses[6]
    $limited = $responses[7]
    $invalidGateFilter = $responses[8]
    $invalidBoundary = $responses[9]
    $invalidLimit = $responses[10]
    $help = $responses[11]

    $signalsArtifact = Read-Artifact $allSignals
    $gatesArtifact = Read-Artifact $allGates
    $nandArtifact = Read-Artifact $nandGates
    $scopeArtifact = Read-Artifact $scoped
    $boundaryArtifact = Read-Artifact $boundary
    $limitedArtifact = Read-Artifact $limited

    Check-Result `
        ($findAny -match "status: ok" -and
         $findAny -match "report_status: MATCH_FOUND" -and
         $findAny -match "search_type: COMPLEMENTARY_PAIRS" -and
         $findAny -match "candidate_domain: SIGNALS" -and
         $findAny -match "proven_complementary: true" -and
         $findAny -match "match_count: 1") `
        "FindAny returns a proven complementary witness"

    Check-Result `
        ($allSignals -match "status: ok" -and
         $allSignals -match "complete: true" -and
         $allSignals -match "complementary_pair_count: [1-9][0-9]*" -and
         $signalsArtifact -match "query_type: COMPLEMENTARY_PAIRS" -and
         $signalsArtifact -match "candidate_domain: SIGNALS" -and
         $signalsArtifact -match "reconstruction: positive_members x negative_members" -and
         $signalsArtifact -match "net_id=" -and
         $signalsArtifact -match "net: and_direct|positive_member [0-9]+: and_direct|negative_member [0-9]+: and_direct" -and
         $signalsArtifact -match "not_q") `
        "signal-domain FindAll writes complete self-contained phase classes"

    Check-Result `
        ($allGates -match "status: ok" -and
         $allGates -match "candidate_domain: COMBINATIONAL_GATE_OUTPUTS" -and
         $allGates -match "candidate_gate_count: [1-9][0-9]*" -and
         $gatesArtifact -match "candidate_domain: COMBINATIONAL_GATE_OUTPUTS" -and
         $gatesArtifact -match "gate=g_and_direct" -and
         $gatesArtifact -match "gate_type=AND") `
        "gate-domain FindAll reports gate identity and type"

    Check-Result `
        ($nandGates -match "status: ok" -and
         $nandGates -match "gate_type_filter: NAND" -and
         $nandGates -match "complementary_pair_count: 1" -and
         $nandArtifact -match "gate=g_nand_direct" -and
         $nandArtifact -match "gate=g_and_via_nand") `
        "gate-type filtering works for complementary NAND outputs"

    Check-Result `
        ($scoped -match "status: ok" -and
         $scoped -match "scope: NET_FANIN" -and
         $scoped -match "scope_name: out_and" -and
         $scopeArtifact -match "scope: NET_FANIN" -and
         $scopeArtifact -notmatch "not_q") `
        "scope restricts candidates without changing Boolean support"

    Check-Result `
        ($boundary -match "status: ok" -and
         $boundaryArtifact -match "include_pi_po_port_signals: true" -and
         $boundaryArtifact -match "positive_member [0-9]+: a|negative_member [0-9]+: a" -and
         $boundaryArtifact -match "out_a") `
        "boundary opt-in adds PI and PO signal candidates"

    Check-Result `
        ($limited -match "status: partial" -and
         $limited -match "report_status: RESULT_LIMIT_REACHED" -and
         $limited -match "complete: false" -and
         $limited -match "truncated: true" -and
         $limitedArtifact -match "max_results: 1" -and
         $limitedArtifact -match "pair_representation: literal_pairs_explicit_limit" -and
         ([regex]::Matches($limitedArtifact, "(?m)^Match [0-9]+\r?$").Count -eq 1) -and
         $limitedArtifact -notmatch "(?m)^Complementary class [0-9]+\r?$") `
        "explicit max-results is reported as partial"

    Check-Result `
        ($invalidGateFilter -match "status: error" -and
         $invalidGateFilter -match "--gate-type is only valid") `
        "gate-type filter is rejected for the signals domain"

    Check-Result `
        ($invalidBoundary -match "status: error" -and
         $invalidBoundary -match "not gate-output searches") `
        "boundary option is rejected for the gates domain"

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
        ($functionSearchHelp -match "complementary_pairs <signals\|gates>" -and
         $functionSearchHelp -notmatch "--patterns" -and
         $functionSearchHelp -notmatch "--time-limit") `
        "public help exposes the mode without backend tuning knobs"
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
