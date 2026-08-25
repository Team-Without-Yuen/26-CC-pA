param(
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $Executable) {
    $Executable = Join-Path $root 'tools.exe'
}
$tools = (Resolve-Path $Executable).Path
$smallCircuit = Join-Path $PSScriptRoot 'ranked_paths.v'
$largeCircuit = Join-Path $PSScriptRoot 'ranked_paths_large.v'
$createdArtifacts = [System.Collections.Generic.List[string]]::new()

function Invoke-Tools([string[]] $Commands) {
    $inputText = (($Commands + 'exit') -join "`n") + "`n"
    return (($inputText | & $tools 2>&1) -join "`n") -replace "`r", ''
}

function Assert-Contains([string] $Text, [string] $Expected, [string] $Label) {
    if (-not $Text.Contains($Expected)) {
        throw "[$Label] Missing expected text: $Expected`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

function Assert-NotContains([string] $Text, [string] $Unexpected, [string] $Label) {
    if ($Text.Contains($Unexpected)) {
        throw "[$Label] Unexpected text: $Unexpected`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

function Assert-True([bool] $Condition, [string] $Label) {
    if (-not $Condition) {
        throw "[$Label] assertion failed"
    }
    Write-Host "[PASS] $Label"
}

function Get-RankBlock([string] $Text, [int] $Rank) {
    $match = [regex]::Match(
        $Text,
        "(?ms)^Rank $Rank`:`n(.*?)(?=^Rank [0-9]+:|^TOOL_RESULT_END)")
    if (-not $match.Success) {
        throw "Missing rank block $Rank`n--- output ---`n$Text"
    }
    return $match.Value
}

function Invoke-Small([string] $Command) {
    return Invoke-Tools @("read $smallCircuit", $Command)
}

try {
    # Same-depth paths use gate-name ordering after depth/start/end ties.
    $shortTie = Invoke-Small 'path_query top_k_shortest pi:a po:z 2'
    Assert-Contains $shortTie 'status: ok' 'top-K shortest succeeds'
    Assert-Contains $shortTie 'Returned ranked paths: 2' 'top-K shortest returns two paths'
    Assert-Contains $shortTie 'Population exhausted: yes' 'exact K reports exhausted population'
    Assert-Contains (Get-RankBlock $shortTie 1) 'g_alpha' 'canonical tie rank 1 uses g_alpha'
    Assert-Contains (Get-RankBlock $shortTie 2) 'g_beta' 'canonical tie rank 2 uses g_beta'
    Assert-Contains (Get-RankBlock $shortTie 1) 'Depth: 2' 'Top-1 shortest has expected extrema depth'
    Assert-Contains $shortTie 'Ranked depth range count: 1' 'same-depth ranks compress to one range'
    Assert-Contains $shortTie 'ranks 1-2: depth=2' 'compressed depth range maps both ranks exactly'
    $shortTieAgain = Invoke-Small 'path_query top_k_shortest pi:a po:z 2'
    Assert-True ((Get-RankBlock $shortTie 1) -eq (Get-RankBlock $shortTieAgain 1)) `
        'canonical rank is stable after reread'

    $nthSecond = Invoke-Small 'path_query nth_shortest pi:a po:z 2'
    Assert-Contains $nthSecond 'Requested first rank: 2' 'Nth keeps original 1-based rank'
    Assert-Contains $nthSecond 'Returned ranked paths: 1' 'Nth returns one path'
    Assert-Contains $nthSecond 'Requested rank depth: 2' 'Nth depth remains scalar decision metadata'
    Assert-Contains $nthSecond 'rank 2: depth=2' 'Nth depth range keeps the requested rank'
    Assert-Contains (Get-RankBlock $nthSecond 2) 'g_beta' 'second shortest is canonical beta path'

    $tooLarge = Invoke-Small 'path_query nth_shortest pi:a po:z 3'
    Assert-Contains $tooLarge 'status: ok' 'missing Nth rank is a valid query'
    Assert-Contains $tooLarge 'Requested rank exists: no' 'missing Nth rank is explicit'
    Assert-Contains $tooLarge 'Population exhausted: yes' 'missing Nth rank is proven'
    Assert-Contains $tooLarge 'Ranked depth range count: 0' 'missing rank has no depth metadata'
    Assert-NotContains $tooLarge 'Requested rank depth:' 'missing rank has no fabricated depth'
    Assert-NotContains $tooLarge 'Rank 3:' 'missing Nth rank has no fabricated path'

    $oversizedK = Invoke-Small 'path_query top_k_longest pi:a po:z 5'
    Assert-Contains $oversizedK 'Returned ranked paths: 2' 'Top-K returns all available paths'
    Assert-Contains $oversizedK 'Population exhausted: yes' 'Top-K shortage is explicit'
    Assert-Contains (Get-RankBlock $oversizedK 1) 'Depth: 2' 'Top-1 longest has expected extrema depth'

    # Global ranking includes all endpoint pairs, including an endpoint upstream of another.
    $global = Invoke-Small 'path_query top_k_shortest all_pi all_po 7'
    Assert-Contains $global 'Returned ranked paths: 7' 'global multi-endpoint population is complete'
    Assert-Contains $global 'Population exhausted: yes' 'global population exhaustion is proven'
    Assert-Contains (Get-RankBlock $global 1) 'g_tied' 'global rank 1 follows endpoint name tie-break'
    Assert-Contains (Get-RankBlock $global 2) 'g_short' 'global rank 2 keeps same-start ordering'
    Assert-Contains (Get-RankBlock $global 3) 'g_b_short' 'global rank 3 follows start-name ordering'
    Assert-Contains (Get-RankBlock $global 4) 'g_alpha' 'upstream endpoint path is retained'
    Assert-Contains (Get-RankBlock $global 6) 'g_after' 'downstream endpoint path is retained'

    $longest = Invoke-Small 'path_query top_k_longest pi:a all_po 2'
    Assert-Contains (Get-RankBlock $longest 1) 'g_alpha' 'longest rank 1 uses canonical alpha path'
    Assert-Contains (Get-RankBlock $longest 1) 'g_after' 'longest rank 1 reaches downstream output'
    Assert-Contains (Get-RankBlock $longest 2) 'g_beta' 'longest rank 2 uses canonical beta path'

    # Stage 1 bounds and structural constraints compose with ranking.
    $depthFiltered = Invoke-Small `
        'path_query top_k_shortest pi:a all_po 5 -depth_eq 3'
    Assert-Contains $depthFiltered 'Returned ranked paths: 2' 'depth predicate filters ranked population'
    Assert-Contains $depthFiltered 'Minimum accepted depth: 3' 'ranked lower depth is reported'
    Assert-Contains $depthFiltered 'Maximum accepted depth: 3' 'ranked upper depth is reported'
    Assert-Contains (Get-RankBlock $depthFiltered 1) 'g_after' 'depth-filtered path is literal'

    $required = Invoke-Small `
        'path_query top_k_shortest pi:a all_po 4 -req gate:g_join'
    Assert-Contains $required 'Returned ranked paths: 4' 'required gate composes with ranking'
    Assert-Contains (Get-RankBlock $required 1) 'g_join' 'required gate is present'

    $avoided = Invoke-Small `
        'path_query top_k_shortest pi:a po:w 2 -avoid gate:g_alpha'
    Assert-Contains $avoided 'Returned ranked paths: 1' 'avoided gate removes one ranked branch'
    Assert-Contains $avoided 'g_beta' 'remaining avoided result uses beta branch'
    Assert-NotContains $avoided 'g_alpha' 'avoided gate is absent'

    $tied = Invoke-Small 'path_query top_k_shortest pi:a po:tied_out 2'
    Assert-Contains $tied 'Returned ranked paths: 1' 'tied input remains one structural ranked path'

    $dffD = Invoke-Small 'path_query top_k_shortest pi:a dff_d:ff 2'
    Assert-Contains $dffD 'Returned ranked paths: 2' 'DFF.D remains a ranked endpoint boundary'
    Assert-Contains $dffD 'Depth: 3' 'DFF.D ranked paths have expected depth'

    # Invalid rank grammar fails closed and never prints a ranked result.
    foreach ($case in @(
        'path_query top_k_shortest pi:a po:z',
        'path_query top_k_shortest pi:a po:z 0',
        'path_query nth_longest pi:a po:z -1',
        'path_query nth_shortest pi:a po:z 1.5',
        'path_query top_k_longest pi:a po:z 18446744073709551616',
        'path_query top_k_shortest pi:a po:z 1 trailing',
        'path_query top_k_shortest pi:a po:z 1 -count_only',
        'path_query top_k_shortest pi:a po:z 1 -out ignored.txt',
        'path_query top_k_shortest pi:a po:z 1 -depth_ge 3 -depth_le 2'
    )) {
        $invalid = Invoke-Small $case
        Assert-Contains $invalid 'status: error' "invalid ranked command rejected: $case"
        Assert-NotContains $invalid 'Returned ranked paths:' `
            "invalid ranked command has no result: $case"
    }

    # 256-path population; explicit K=200 must be complete and self-contained in an artifact.
    $large = Invoke-Tools @(
        "read $largeCircuit",
        'path_query top_k_shortest pi:a po:y 200'
    )
    Assert-Contains $large 'status: ok' 'large ranked query succeeds'
    Assert-Contains $large 'Returned ranked paths: 200' 'large ranked query returns requested K'
    Assert-Contains $large 'Population exhausted: no' 'large ranked query reports remaining population'
    Assert-Contains $large 'Returned first rank depth: 16' 'large envelope keeps first returned depth'
    Assert-Contains $large 'Returned last rank depth: 16' 'large envelope keeps last returned depth'
    Assert-Contains $large 'Ranked depth range count: 1' 'large envelope keeps exact range count'
    Assert-Contains $large 'ranks 1-200: depth=16' 'large envelope keeps compressed rank-depth mapping'
    Assert-Contains $large 'list artifact complete: yes' 'large ranked result uses complete artifact'
    Assert-NotContains $large 'Rank 1:' 'large ranked paths stay out of terminal'
    $artifactMatch = [regex]::Match($large, '(?m)^  output_file: (.+)$')
    Assert-True $artifactMatch.Success 'large ranked response provides artifact path'
    $artifactPath = $artifactMatch.Groups[1].Value.Trim()
    if (-not [System.IO.Path]::IsPathRooted($artifactPath)) {
        $artifactPath = Join-Path $root $artifactPath
    }
    $createdArtifacts.Add($artifactPath)
    Assert-True (Test-Path -LiteralPath $artifactPath) 'large ranked artifact exists'
    $artifactLines = @(Get-Content -LiteralPath $artifactPath)
    $artifactText = $artifactLines -join "`n"
    $rankRecords = @($artifactLines | Where-Object { $_ -match '^  rank [0-9]+: depth=' })
    Assert-Contains $artifactText 'ranking_order: shortest_first' 'artifact records ranking order'
    Assert-Contains $artifactText 'requested_result_count: 200' 'artifact records requested K'
    Assert-Contains $artifactText 'population_exhausted: no' 'artifact records population status'
    Assert-Contains $artifactText 'first_returned_rank_depth: 16' 'artifact records first returned depth'
    Assert-Contains $artifactText 'last_returned_rank_depth: 16' 'artifact records last returned depth'
    Assert-Contains $artifactText 'Ranked path depth ranges (1):' 'artifact records compressed depth mapping'
    Assert-Contains $artifactText 'ranks 1-200: depth=16' 'artifact depth mapping is exact'
    Assert-Contains $artifactText 'rank 1: depth=16 path=a ->' 'artifact paths are self-contained literals'
    Assert-Contains $artifactText 'rank 200: depth=16 path=a ->' 'artifact preserves final requested rank'
    Assert-True ($rankRecords.Count -eq 200) 'artifact contains exactly 200 ranked records'

    $timedOut = Invoke-Tools @(
        "read $largeCircuit",
        'path_query top_k_shortest pi:a po:y 200 -time_limit 0.000001'
    )
    Assert-Contains $timedOut 'status: timeout' 'ranked timeout has timeout status'
    Assert-Contains $timedOut 'complete: false' 'ranked timeout envelope is incomplete'
    Assert-Contains $timedOut 'Ranking complete: no' 'ranked timeout is not a proof'
    Assert-Contains $timedOut 'Returned ranked paths: 0' 'ranked timeout exposes no unsafe partial rank'
    Assert-Contains $timedOut 'Ranked depth range count: 0' 'ranked timeout exposes no depth ranges'
    Assert-NotContains $timedOut 'Requested rank depth:' 'ranked timeout exposes no unsafe depth'
    Assert-NotContains $timedOut 'Rank 1:' 'ranked timeout fabricates no rank'

    Write-Host '[PASS] PATH-HIDDEN-005 Stage 2 regression completed'
}
finally {
    foreach ($artifactPath in $createdArtifacts) {
        Remove-Item -LiteralPath $artifactPath -Force -ErrorAction SilentlyContinue
    }
}
