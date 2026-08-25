param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test26/equivalent_gate_pairs_circuit.v"
    "func_search equivalent_pairs whole"
    "func_search equivalent_pairs whole --all --max-results 64 --patterns 128 --time-limit 5"
    "func_search equivalent_pairs whole --all --max-results 2"
    "func_search equivalent_pairs whole --all --gate-type AND"
    "func_search equivalent_pairs net_fanin y --all"
    "func_search equivalent_pairs net_fanin z --all"
    "func_search equivalent_pairs unknown_scope"
    "func_search equivalent_pairs whole --gate-type DFF"
    "func_search equivalent_pairs whole --allow-same"
    "read mini test/test26/equivalent_gate_pairs_circuit.v"
    "edit_apply merge_functionally_equivalent_gates whole --patterns 256 --time-limit 5"
    "report_query last_edit"
    "func_search equivalent_pairs whole --all"
    "edit_apply merge_functionally_equivalent_gates whole --time-limit 5"
    "read mini test/test26/equivalent_gate_pairs_circuit.v"
    "edit_apply merge_functionally_equivalent_gates net_fanin y --time-limit 5"
    "read mini test/test26/equivalent_gate_pairs_circuit.v"
    "edit_apply merge_functionally_equivalent_gates whole --time-limit 0.000000000001"
    "read mini test/test26/functional_merge_rollback_circuit.v"
    "edit_apply merge_functionally_equivalent_gates whole --time-limit 5"
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

Check-Result ($responses.Count -eq 23) "every equivalent-pair search/edit command returns one envelope"
if ($responses.Count -ge 23) {
    $findAny = $responses[1]
    $findAll = $responses[2]
    $limited = $responses[3]
    $andOnly = $responses[4]
    $yCone = $responses[5]
    $zCone = $responses[6]
    $badScope = $responses[7]
    $badType = $responses[8]
    $badOption = $responses[9]
    $merge = $responses[11]
    $cachedMerge = $responses[12]
    $postMergeSearch = $responses[13]
    $noChangeMerge = $responses[14]
    $scopedMerge = $responses[16]
    $timeoutMerge = $responses[18]
    $rollbackMerge = $responses[20]
    $help = $responses[21]
    $findAllFile = Get-OutputFile $findAll
    $limitedFile = Get-OutputFile $limited
    $andOnlyFile = Get-OutputFile $andOnly
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
    $andOnlyFileExists = [bool]($andOnlyFile -and (Test-Path -LiteralPath $andOnlyFile))
    $andOnlyArtifact = if ($andOnlyFileExists) {
        Get-Content -LiteralPath $andOnlyFile -Raw
    } else {
        ""
    }

    Check-Result `
        ($findAny -match "status: ok" -and
         $findAny -match "search_type: EQUIVALENT_GATE_PAIRS" -and
         $findAny -match "report_status: MATCH_FOUND" -and
         $findAny -match "gate_a: " -and
         $findAny -match "solver_status: UNSAT") `
        "FindAny returns a SAT-proven gate-pair witness"

    Check-Result `
        ($findAll -match "status: ok" -and
         $findAll -match "equivalence_class_count: 2" -and
         $findAll -match "equivalent_pair_count: 7" -and
         $findAll -match "match_count: 7" -and
         $findAll -match "stored_match_count: 0" -and
         $findAll -match "wrote_matches_to_file: true" -and
         $findAllFileExists -and
         $findAllArtifact -match "artifact_format: FUNCTION_SEARCH_ARTIFACT_V2" -and
         $findAllArtifact -match "query_type: EQUIVALENT_GATE_PAIRS" -and
         $findAllArtifact -match "search_mode: FIND_ALL" -and
         $findAllArtifact -match "target: not_applicable" -and
         $findAllArtifact -match "scope: WHOLE_DESIGN" -and
         $findAllArtifact -match "gate_type_filter: ANY" -and
         $findAllArtifact -match "candidate_domain: active_combinational_gate_outputs" -and
         $findAllArtifact -match "result_policy: explicit_max_results" -and
         $findAllArtifact -match "max_results: 64" -and
         $findAllArtifact -match "member_count: 4" -and
         $findAllArtifact -match "records_end" -and
         $findAllArtifact -match "equivalence_class_count: 2" -and
         $findAllFileMatchCount -eq 7) `
        "FindAll writes complete classes and expanded pairs to an artifact"

    Check-Result `
        ($limited -match "status: partial" -and
         $limited -match "report_status: RESULT_LIMIT_REACHED" -and
         $limited -match "equivalence_class_count: 2" -and
         $limited -match "equivalent_pair_count: 7" -and
         $limited -match "match_count: 7" -and
         $limitedFileExists -and
         $limitedArtifact -match "result_policy: explicit_max_results" -and
         $limitedArtifact -match "max_results: 2" -and
         $limitedArtifact -match "truncated: true" -and
         $limitedArtifact -match "Complete: no" -and
         $limitedFileMatchCount -eq 2) `
        "result limits truncate pair output without losing class statistics"

    Check-Result `
        ($andOnly -match "gate_type_filter: AND" -and
         $andOnly -match "equivalent_pair_count: 1" -and
         $andOnlyFileExists -and
         $andOnlyArtifact -match "gate_type_filter: AND" -and
         $andOnlyArtifact -match "result_policy: all_matches" -and
         $andOnlyArtifact -match "max_results: unlimited" -and
         $andOnlyArtifact -match "member_count: 2") `
        "gate-type filter reaches the public report"

    Check-Result `
        ($yCone -match "scope: NET_FANIN" -and
         $yCone -match "scope_name: y" -and
         $yCone -match "equivalent_pair_count: 1") `
        "net_fanin scope limits the search"

    Check-Result `
        ($zCone -match "status: ok" -and
         $zCone -match "report_status: NO_MATCH" -and
         $zCone -match "complete: true") `
        "scoped no-match remains a complete answer"

    Check-Result `
        ($badScope -match "status: error" -and
         $badScope -match "Unknown equivalent_pairs scope") `
        "unknown scopes are rejected by the parser"

    Check-Result `
        ($badType -match "status: error" -and
         $badType -match "supported combinational gate type") `
        "DFF gate filters are rejected"

    Check-Result `
        ($badOption -match "status: error" -and
         $badOption -match "--allow-same is only valid for nand_pair or binary pattern searches") `
        "mode-specific options cannot leak across search types"

    Check-Result `
        ($merge -match "status: ok" -and
         $merge -match "complete: true" -and
         $merge -match "operation_name: edit_apply:merge_functionally_equivalent_gates" -and
         $merge -match "report_success: true" -and
         $merge -match "report_changed: true" -and
         $merge -match "equivalence_method: CertifiedRewrite" -and
         $merge -match "whole_design_equivalence_checked: false" -and
         $merge -match "whole_design_equivalent: false" -and
         $merge -match "merged_gate_count: 4" -and
         $merge -match "merge_record_count: 4") `
        "public functional merge rewires all SAT-proven classes without final whole-design SAT"

    Check-Result `
        ($cachedMerge -match "status: ok" -and
         $cachedMerge -match "operation_name: edit_apply:merge_functionally_equivalent_gates" -and
         $cachedMerge -match "merged_gate_count: 4") `
        "report_query preserves detailed functional merge outcomes"

    Check-Result `
        ($postMergeSearch -match "status: ok" -and
         $postMergeSearch -match "report_status: NO_MATCH" -and
         $postMergeSearch -match "complete: true") `
        "post-merge search confirms no equivalent class remains"

    Check-Result `
        ($noChangeMerge -match "status: no_change" -and
         $noChangeMerge -match "report_success: true" -and
         $noChangeMerge -match "report_changed: false" -and
         $noChangeMerge -match "merged_gate_count: 0") `
        "repeating functional merge returns a certified no-change response"

    Check-Result `
        ($scopedMerge -match "status: ok" -and
         $scopedMerge -match "scope: NET_FANIN" -and
         $scopedMerge -match "scope_name: y" -and
         $scopedMerge -match "merged_gate_count: 1" -and
         $scopedMerge -match "equivalence_method: CertifiedRewrite" -and
         $scopedMerge -match "whole_design_equivalence_checked: false") `
        "public functional merge respects net_fanin scope"

    Check-Result `
        ($timeoutMerge -match "status: timeout" -and
         $timeoutMerge -match "complete: false" -and
         $timeoutMerge -match "report_success: false" -and
         $timeoutMerge -match "report_changed: false" -and
         $timeoutMerge -match "search_timed_out: true") `
        "functional merge timeout occurs before mutation and is explicit"

    Check-Result `
        ($rollbackMerge -match "status: ok" -and
         $rollbackMerge -match "complete: true" -and
         $rollbackMerge -match "report_success: true" -and
         $rollbackMerge -match "report_changed: true" -and
         $rollbackMerge -match "rolled_back: false" -and
         $rollbackMerge -match "merged_gate_count: 1" -and
         $rollbackMerge -match "equivalence_method: CertifiedRewrite" -and
         $rollbackMerge -match "whole_design_equivalence_checked: false") `
        "functional merge does not require endpoint-based whole-design SAT"

    Check-Result `
        ($help -match "func_search equivalent_pairs <scope>" -and
         $help -match "--gate-type type" -and
         $help -match "gate_fanout <gate>" -and
         $help -match "edit_apply merge_functionally_equivalent_gates <scope>") `
        "help exposes equivalent-pair search and merge grammar"
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
