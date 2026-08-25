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
    "func_search pattern NOT not_a --include-boundary-signals"
    "func_search pattern AND and_ab --include-boundary-signals --allow-same"
    "func_search pattern AND and_ab --include-boundary-signals --scope net_fanin and_ab"
    "func_search pattern OR target --all --include-boundary-signals"
    "func_search pattern DFF target"
    "func_search pattern NOT not_a --allow-same"
    "func_search nand_pair target --all --find-any"
    "func_search pattern AND and_ab --include-boundary-signals -include_boundary_signals"
    "func_search nand_pair target --max-results 2"
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

function Get-OutputFile {
    param([string]$Response)
    $match = [regex]::Match($Response, "(?m)^  output_file: (.+)$")
    if (!$match.Success) { return $null }
    return $match.Groups[1].Value.Trim()
}

function Get-IntegerField {
    param([string]$Response, [string]$Field)
    $match = [regex]::Match(
        $Response,
        "(?m)^  " + [regex]::Escape($Field) + ": ([0-9]+)\r?$")
    if (!$match.Success) { return $null }
    return [int64]$match.Groups[1].Value
}

Check-Result ($responses.Count -eq 19) "every Function Search command returns one envelope"
if ($responses.Count -ge 19) {
    $findAny = $responses[1]
    $findAll = $responses[2]
    $limited = $responses[3]
    $noMatch = $responses[4]
    $missing = $responses[5]
    $invalid = $responses[6]
    $timeout = $responses[7]
    $patternNot = $responses[8]
    $patternAnd = $responses[9]
    $patternScoped = $responses[10]
    $patternOrAll = $responses[11]
    $invalidPattern = $responses[12]
    $invalidUnaryOption = $responses[13]
    $conflictingMode = $responses[14]
    $duplicateOption = $responses[15]
    $findAnyLimit = $responses[16]
    $help = $responses[17]
    $functionSearchHelpMatch = [regex]::Match(
        $help,
        "(?s)Function search\r?\n(.*?)\r?\nSequential pattern query")
    $functionSearchHelp = if ($functionSearchHelpMatch.Success) {
        $functionSearchHelpMatch.Groups[1].Value
    } else {
        ""
    }
    $findAllFile = Get-OutputFile $findAll
    $limitedFile = Get-OutputFile $limited
    $findAllMatchCount = Get-IntegerField $findAll "match_count"
    $findAllFileExists = [bool]($findAllFile -and (Test-Path -LiteralPath $findAllFile))
    $findAllArtifact = if ($findAllFileExists) {
        Get-Content -LiteralPath $findAllFile -Raw
    } else {
        ""
    }
    $findAllFileMatchCount = if ($findAllFileExists) {
        @(Select-String -LiteralPath $findAllFile -Pattern '^Match [0-9]+$').Count
    } else {
        -1
    }
    $limitedFileExists = [bool]($limitedFile -and (Test-Path -LiteralPath $limitedFile))
    $limitedArtifact = if ($limitedFileExists) {
        Get-Content -LiteralPath $limitedFile -Raw
    } else {
        ""
    }
    $limitedFileMatchCount = if ($limitedFileExists) {
        @(Select-String -LiteralPath $limitedFile -Pattern '^Match [0-9]+$').Count
    } else {
        -1
    }
    $patternOrFile = Get-OutputFile $patternOrAll
    $patternOrFileExists = [bool]($patternOrFile -and (Test-Path -LiteralPath $patternOrFile))
    $patternOrArtifact = if ($patternOrFileExists) {
        Get-Content -LiteralPath $patternOrFile -Raw
    } else {
        ""
    }

    Check-Result `
        ($findAny -match "status: ok" -and
         $findAny -match "report_status: MATCH_FOUND" -and
         $findAny -match "found: true" -and
         $findAny -match "complete: true" -and
         $findAny -match "proof_method: AIG_(LITERAL_EQUALITY|INCREMENTAL_SAT)" -and
         $findAny -match "solver_status: UNSAT") `
        "FindAny returns a complete SAT-proven witness"

    Check-Result `
        ($findAll -match "status: ok" -and
         $findAll -match "report_status: MATCHES_FOUND" -and
         $findAll -match "all_candidates_examined: true" -and
         $findAll -match "truncated: false" -and
         $findAllMatchCount -gt 0 -and
         $findAll -match "stored_match_count: 0" -and
         $findAll -match "wrote_matches_to_file: true" -and
         $findAllFileExists -and
         $findAllArtifact -match "artifact_format: FUNCTION_SEARCH_ARTIFACT_V2" -and
         $findAllArtifact -match "query_type: NAND_EQUIVALENT_INPUT_PAIRS" -and
         $findAllArtifact -match "search_mode: FIND_ALL" -and
         $findAllArtifact -match "target: target" -and
         $findAllArtifact -match "scope: WHOLE_DESIGN" -and
         $findAllArtifact -match "gate_type_filter: not_applicable" -and
         $findAllArtifact -match "candidate_domain: driven_non_port_signals_including_internal_dff_q" -and
         $findAllArtifact -match "include_pi_po_port_signals: false" -and
         $findAllArtifact -match "internal_dff_q_signals_included: true" -and
         $findAllArtifact -match "dff_q_output_port_requires_boundary_opt_in: true" -and
         $findAllArtifact -match "result_policy: explicit_max_results" -and
         $findAllArtifact -match "max_results: 64" -and
         $findAllArtifact -match "records_end" -and
         $findAllArtifact -match "all_candidates_examined: true" -and
         $findAllFileMatchCount -eq $findAllMatchCount) `
        "FindAll writes every SAT-proven match to a complete artifact"

    Check-Result `
        ($limited -match "status: partial" -and
         $limited -match "complete: false" -and
         $limited -match "report_status: RESULT_LIMIT_REACHED" -and
         $limited -match "truncated: true" -and
         $limited -match "match_count: 1" -and
         $limitedFileExists -and
         $limitedArtifact -match "result_policy: explicit_max_results" -and
         $limitedArtifact -match "max_results: 1" -and
         $limitedArtifact -match "truncated: true" -and
         $limitedArtifact -match "Complete: no" -and
         $limitedFileMatchCount -eq 1) `
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
        ($patternNot -match "status: ok" -and
         $patternNot -match "search_type: FUNCTIONAL_PATTERN_OPERANDS" -and
         $patternNot -match "pattern_type: NOT" -and
         $patternNot -match "operand_arity: 1" -and
         $patternNot -match "operand_count: 1" -and
         $patternNot -match "solver_status: UNSAT") `
        "generic unary pattern search returns one proven operand"

    Check-Result `
        ($patternAnd -match "status: ok" -and
         $patternAnd -match "pattern_type: AND" -and
         $patternAnd -match "operand_arity: 2" -and
         $patternAnd -match "operand_count: 2" -and
         $patternAnd -match "solver_status: UNSAT") `
        "generic binary pattern search returns two proven operands"

    Check-Result `
        ($patternScoped -match "status: ok" -and
         $patternScoped -match "scope: NET_FANIN" -and
         $patternScoped -match "scope_name: and_ab" -and
         $patternScoped -match "found: true" -and
         $patternScoped -match "operand_count: 2") `
        "generic pattern scope includes valid cone-boundary operands"

    Check-Result `
        ($patternOrAll -match "status: ok" -and
         $patternOrAll -match "pattern_type: OR" -and
         $patternOrAll -match "complete: true" -and
         $patternOrAll -match "wrote_matches_to_file: true" -and
         $patternOrFileExists -and
         $patternOrArtifact -match "artifact_format: FUNCTION_SEARCH_ARTIFACT_V2" -and
         $patternOrArtifact -match "query_type: FUNCTIONAL_PATTERN_OPERANDS" -and
         $patternOrArtifact -match "pattern: OR" -and
         $patternOrArtifact -match "operand_arity: 2" -and
         $patternOrArtifact -match "include_boundary_signals: true" -and
         $patternOrArtifact -match "include_pi_po_port_signals: true" -and
         $patternOrArtifact -match "internal_dff_q_signals_included: true" -and
         $patternOrArtifact -match "allow_same_signal_pair: false" -and
         $patternOrArtifact -match "result_policy: all_matches" -and
         $patternOrArtifact -match "max_results: unlimited") `
        "generic FindAll writes a self-describing artifact"

    Check-Result `
        ($invalidPattern -match "status: error" -and
         $invalidPattern -match "pattern gate type must be") `
        "generic pattern parser rejects DFF"

    Check-Result `
        ($invalidUnaryOption -match "status: error" -and
         $invalidUnaryOption -match "not meaningful for unary") `
        "generic pattern parser rejects binary-only option for unary search"

    Check-Result `
        ($conflictingMode -match "status: error" -and
         $conflictingMode -match "--all and --find-any are mutually exclusive") `
        "conflicting search modes are rejected instead of using last-option-wins"

    Check-Result `
        ($duplicateOption -match "status: error" -and
         $duplicateOption -match "Duplicate func_search option: --include-boundary-signals") `
        "canonical and alias spellings of the same option are rejected as duplicates"

    Check-Result `
        ($findAnyLimit -match "status: error" -and
         $findAnyLimit -match "--max-results is only valid with --all") `
        "FindAny rejects a meaningless result limit"

    Check-Result `
        ($functionSearchHelp -match "func_search nand_pair <target_net>" -and
         $functionSearchHelp -match "func_search pattern <BUF\|NOT\|AND\|NAND\|OR\|NOR\|XOR\|XNOR>" -and
         $functionSearchHelp -match "--all" -and
         $functionSearchHelp -notmatch "--patterns" -and
         $functionSearchHelp -notmatch "--time-limit") `
        "public help exposes only LLM-facing Function Search grammar"
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
