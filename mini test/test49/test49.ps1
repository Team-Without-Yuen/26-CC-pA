$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tools = Join-Path $root 'tools.exe'
$circuit = Join-Path $PSScriptRoot 'cone_filter.v'

function Invoke-Tools([string[]] $Commands) {
    $inputText = (($Commands + 'exit') -join "`n") + "`n"
    return ($inputText | & $tools 2>&1) -join "`n"
}

function Assert-Contains([string] $Text, [string] $Expected, [string] $Label) {
    if (-not $Text.Contains($Expected)) {
        throw "[$Label] Missing expected text: $Expected`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

function Assert-NotContains([string] $Text, [string] $Unexpected, [string] $Label) {
    if ($Text.Contains($Unexpected)) {
        throw "[$Label] Unexpected text present: $Unexpected`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

$read = "read $circuit"

$multi = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --gate-types and OR and --with-pins'
)
Assert-Contains $multi 'status: ok' 'multi-type command succeeds'
Assert-Contains $multi 'scope gates: 8' 'scope count remains unfiltered'
Assert-Contains $multi 'filtered gates: 2' 'AND/OR union count'
Assert-Contains $multi 'gate type filters: AND, OR' 'filters normalized and deduplicated'
Assert-Contains $multi 'gate detail count: 2' 'structured detail count'
Assert-Contains $multi 'gate=g_and type=AND inputs=[IN1=a(PI), IN2=b(PI)] output=OUT=n_and' 'AND pin detail present'
Assert-Contains $multi 'gate=g_or type=OR inputs=[IN1=n_and, IN2=c(PI)] output=OUT=n_or' 'OR pin detail present'

$all = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --gate-types AND OR NOT NAND NOR XOR XNOR BUF DFF'
)
Assert-Contains $all 'filtered gates: 8' 'all combinational types retained'
Assert-Contains $all 'gate type filters: AND, OR, NAND, NOR, NOT, BUF, XOR, XNOR, DFF' 'all legal parser types accepted'

$dff = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --gate-types DFF'
)
Assert-Contains $dff 'status: ok' 'DFF filter is valid input'
Assert-Contains $dff 'filtered gates: 0' 'DFF excluded at sequential cone boundary'
Assert-Contains $dff 'complete: true' 'valid zero result is complete'

$invalid = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --gate-types MUX'
)
Assert-Contains $invalid 'status: error' 'unsupported type rejected'
Assert-Contains $invalid "Unsupported gate type 'MUX'" 'unsupported type diagnostic'
Assert-Contains $invalid 'AND OR NOT NAND NOR XOR XNOR BUF DFF' 'supported type list reported'

$missing = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --gate-types'
)
Assert-Contains $missing 'status: error' 'missing type rejected'
Assert-Contains $missing '--gate-types requires at least one gate type' 'missing type diagnostic'

$unknownOption = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --mystery'
)
Assert-Contains $unknownOption 'status: error' 'unknown option rejected'
Assert-Contains $unknownOption 'Unknown cone_query option: --mystery' 'unknown option diagnostic'

$missingNetOperand = Invoke-Tools @(
    $read,
    'cone_query net_fanin --with-pins'
)
Assert-Contains $missingNetOperand 'status: error' 'option is not accepted as a net operand'
Assert-Contains $missingNetOperand 'net_fanin requires <net>' 'missing net operand diagnostic'

$missingGateOperand = Invoke-Tools @(
    $read,
    'cone_query gate_fanout --gate-types AND'
)
Assert-Contains $missingGateOperand 'status: error' 'option is not accepted as a gate operand'
Assert-Contains $missingGateOperand 'gate_fanout requires <gate>' 'missing gate operand diagnostic'

$missingSecondSharedNet = Invoke-Tools @(
    $read,
    'cone_query shared_fanin y --with-paths'
)
Assert-Contains $missingSecondSharedNet 'status: error' 'option is not accepted as the second shared net'
Assert-Contains $missingSecondSharedNet 'shared_fanin requires <net_a> <net_b>' 'missing shared net diagnostic'

$orderedOptions = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --with-pins --gate-types NAND NOR --with-paths'
)
Assert-Contains $orderedOptions 'status: ok' 'options accepted in mixed order'
Assert-Contains $orderedOptions 'filtered gates: 2' 'NAND/NOR union count'
Assert-Contains $orderedOptions 'longest local path depth:' 'path option preserved'

$highest = Invoke-Tools @(
    $read,
    'cone_query output_rank gates highest'
)
Assert-Contains $highest 'status: ok' 'output rank highest succeeds'
Assert-Contains $highest 'output cone ranking metric: scope_gates' 'scope-gate metric reported'
Assert-Contains $highest 'result output count: 1' 'highest level has one output'
Assert-Contains $highest 'rank=1 output=y metric=8 scope_gates=8 filtered_gates=8 nets=11' 'highest output summary is self-contained'

$top = Invoke-Tools @(
    $read,
    'cone_query output_rank gates top 2'
)
Assert-Contains $top 'selected cone metric levels: 2' 'top K counts distinct levels'
Assert-Contains $top 'result output count: 2' 'top two levels retain both outputs'
Assert-Contains $top 'rank=2 output=q metric=0' 'DFF Q cone remains a zero-gate boundary'

$filteredRank = Invoke-Tools @(
    $read,
    'cone_query output_rank filtered_gates highest --gate-types OR'
)
Assert-Contains $filteredRank 'output cone ranking metric: filtered_gates' 'filtered-gate metric reported'
Assert-Contains $filteredRank 'rank=1 output=y metric=1 scope_gates=8 filtered_gates=1' 'filtered rank uses the selected gate types'

$invalidRanking = Invoke-Tools @(
    $read,
    'cone_query output_rank depth highest'
)
Assert-Contains $invalidRanking 'status: error' 'unsupported output rank metric rejected'
Assert-Contains $invalidRanking 'metric must be gates, filtered_gates, or nets' 'unsupported metric diagnostic'

$missingRankValue = Invoke-Tools @(
    $read,
    'cone_query output_rank gates nth_highest'
)
Assert-Contains $missingRankValue 'status: error' 'missing output rank value rejected'
Assert-Contains $missingRankValue 'requires a positive integer <k>' 'missing rank diagnostic'

$greaterFilter = Invoke-Tools @(
    $read,
    'cone_query output_filter gates gt 0'
)
Assert-Contains $greaterFilter 'status: ok' 'output filter greater-than succeeds'
Assert-Contains $greaterFilter 'output cone filter metric: scope_gates' 'output filter metric reported'
Assert-Contains $greaterFilter 'output cone filter predicate: greater_than' 'output filter predicate reported'
Assert-Contains $greaterFilter 'checked primary outputs: 2' 'output filter checks every active output'
Assert-Contains $greaterFilter 'matched output count: 1' 'output filter matched count reported'
Assert-Contains $greaterFilter 'output=y metric=8 scope_gates=8 filtered_gates=8 nets=11' 'output filter summary is self-contained'

$predicateMatrix = Invoke-Tools @(
    $read,
    'cone_query output_filter gates eq 0',
    'cone_query output_filter gates ne 0',
    'cone_query output_filter gates ge 8',
    'cone_query output_filter gates lt 1',
    'cone_query output_filter gates le 0',
    'cone_query output_filter gates between 0 8'
)
Assert-Contains $predicateMatrix 'output cone filter predicate: equal' 'output filter equal accepted'
Assert-Contains $predicateMatrix 'output cone filter predicate: not_equal' 'output filter not-equal accepted'
Assert-Contains $predicateMatrix 'output cone filter predicate: greater_or_equal' 'output filter greater-or-equal accepted'
Assert-Contains $predicateMatrix 'output cone filter predicate: less_than' 'output filter less-than accepted'
Assert-Contains $predicateMatrix 'output cone filter predicate: less_or_equal' 'output filter less-or-equal accepted'
Assert-Contains $predicateMatrix 'output cone filter predicate: between_inclusive' 'output filter between accepted'
Assert-Contains $predicateMatrix 'filter upper value: 8' 'output filter inclusive upper bound reported'

$filteredOutputFilter = Invoke-Tools @(
    $read,
    'cone_query output_filter filtered_gates ge 1 --gate-types OR'
)
Assert-Contains $filteredOutputFilter 'output cone filter metric: filtered_gates' 'output filter filtered metric reported'
Assert-Contains $filteredOutputFilter 'gate type filters: OR' 'output filter gate type reported'
Assert-Contains $filteredOutputFilter 'matched output count: 1' 'output filter filtered match count'
Assert-Contains $filteredOutputFilter 'output=y metric=1 scope_gates=8 filtered_gates=1' 'output filter uses filtered gate count'

$netOutputFilter = Invoke-Tools @(
    $read,
    'cone_query output_filter nets between 1 1'
)
Assert-Contains $netOutputFilter 'output cone filter metric: nets' 'output filter net metric reported'
Assert-Contains $netOutputFilter 'output=q metric=1 scope_gates=0 filtered_gates=0 nets=1' 'output filter net match preserves DFF boundary'

$invalidOutputFilters = Invoke-Tools @(
    $read,
    'cone_query output_filter depth gt 0',
    'cone_query output_filter gates outside 0',
    'cone_query output_filter gates gt',
    'cone_query output_filter gates between 1',
    'cone_query output_filter gates between 2 1',
    'cone_query output_filter gates gt -1',
    'cone_query output_filter gates gt 0 --with-pins'
)
Assert-Contains $invalidOutputFilters 'output_filter metric must be gates, filtered_gates, or nets' 'output filter rejects unknown metric'
Assert-Contains $invalidOutputFilters 'output_filter predicate must be eq, ne, gt, ge, lt, le, or between' 'output filter rejects unknown predicate'
Assert-Contains $invalidOutputFilters 'output_filter requires <metric> <predicate> <value> [upper]' 'output filter rejects missing value'
Assert-Contains $invalidOutputFilters 'output_filter between requires a non-negative integer <upper>' 'output filter rejects missing upper bound'
Assert-Contains $invalidOutputFilters 'output_filter lower bound must not exceed upper bound' 'output filter rejects inverted range'
Assert-Contains $invalidOutputFilters 'output_filter value must be a non-negative integer' 'output filter rejects negative value'
Assert-Contains $invalidOutputFilters 'output_filter is summary-only' 'output filter rejects detail options'

$largeCircuit = Join-Path $PSScriptRoot 'cone_ranking_large.v'
$largeRanking = Invoke-Tools @(
    "read $largeCircuit",
    'cone_query output_rank gates lowest'
)
Assert-Contains $largeRanking 'result output count: 299' 'large ranking preserves every tied output'
Assert-Contains $largeRanking 'list artifact complete: yes' 'large ranking uses a complete artifact'
Assert-NotContains $largeRanking 'Ranked output cones (299):' 'large ranking list is suppressed from terminal'

$artifactMatch = [regex]::Match(($largeRanking -replace "`r", ''), '(?m)^  output_file: (.+)$')
if (-not $artifactMatch.Success) {
    throw "[large ranking artifact path] Missing output_file`n--- output ---`n$largeRanking"
}
$artifactPath = $artifactMatch.Groups[1].Value.Trim()
if (-not [IO.Path]::IsPathRooted($artifactPath)) {
    $artifactPath = Join-Path $root $artifactPath
}
if (-not (Test-Path -LiteralPath $artifactPath)) {
    throw "[large ranking artifact file] File does not exist: $artifactPath"
}
$artifactText = Get-Content -LiteralPath $artifactPath -Raw
Assert-Contains $artifactText 'Ranked output cones (299):' 'ranking artifact declares the complete tied list'
Assert-Contains $artifactText 'output=y[1] metric=0 scope_gates=0 filtered_gates=0 nets=1' 'ranking artifact contains y[1]'
Assert-Contains $artifactText 'output=y[299] metric=0 scope_gates=0 filtered_gates=0 nets=1' 'ranking artifact contains y[299]'
Assert-Contains $artifactText 'Complete: yes' 'ranking artifact footer declares completeness'
Remove-Item -LiteralPath $artifactPath -Force

$largeFilter = Invoke-Tools @(
    "read $largeCircuit",
    'cone_query output_filter gates eq 0'
)
Assert-Contains $largeFilter 'matched output count: 299' 'large output filter preserves every match'
Assert-Contains $largeFilter 'list artifact complete: yes' 'large output filter uses a complete artifact'
Assert-NotContains $largeFilter 'Matched output cones (299):' 'large output filter list is suppressed from terminal'

$filterArtifactMatch = [regex]::Match(($largeFilter -replace "`r", ''), '(?m)^  output_file: (.+)$')
if (-not $filterArtifactMatch.Success) {
    throw "[large output filter artifact path] Missing output_file`n--- output ---`n$largeFilter"
}
$filterArtifactPath = $filterArtifactMatch.Groups[1].Value.Trim()
if (-not [IO.Path]::IsPathRooted($filterArtifactPath)) {
    $filterArtifactPath = Join-Path $root $filterArtifactPath
}
if (-not (Test-Path -LiteralPath $filterArtifactPath)) {
    throw "[large output filter artifact file] File does not exist: $filterArtifactPath"
}
$filterArtifactText = Get-Content -LiteralPath $filterArtifactPath -Raw
Assert-Contains $filterArtifactText 'Matched output cones (299):' 'output filter artifact declares every match'
Assert-Contains $filterArtifactText 'output=y[1] metric=0 scope_gates=0 filtered_gates=0 nets=1' 'output filter artifact contains y[1]'
Assert-Contains $filterArtifactText 'output=y[299] metric=0 scope_gates=0 filtered_gates=0 nets=1' 'output filter artifact contains y[299]'
Assert-Contains $filterArtifactText 'Complete: yes' 'output filter artifact footer declares completeness'
Remove-Item -LiteralPath $filterArtifactPath -Force

$officialTieCircuit = Join-Path $root 'NewTestCase\test76\test76.v'
$officialTie = Invoke-Tools @(
    "read $officialTieCircuit",
    'cone_query largest_output',
    'cone_query output_rank gates highest',
    'cone_query output_filter gates ge 4'
)
Assert-Contains $officialTie 'message: Largest primary output fanin cone' 'official legacy largest-output query succeeds'
Assert-Contains $officialTie '  source: n4[0]' 'legacy largest-output tie rule remains unchanged'
Assert-Contains $officialTie 'result output count: 3' 'official ranking preserves all highest ties'
Assert-Contains $officialTie 'rank=1 output=n4[0] metric=4 scope_gates=4 filtered_gates=4 nets=7' 'official ranking contains n4[0]'
Assert-Contains $officialTie 'rank=1 output=n5[0] metric=4 scope_gates=4 filtered_gates=4 nets=7' 'official ranking contains n5[0]'
Assert-Contains $officialTie 'rank=1 output=n5[1] metric=4 scope_gates=4 filtered_gates=4 nets=7' 'official ranking contains n5[1]'
Assert-Contains $officialTie 'matched output count: 3' 'official output filter preserves all threshold matches'
Assert-Contains $officialTie 'output=n4[0] metric=4 scope_gates=4 filtered_gates=4 nets=7' 'official output filter contains n4[0]'
Assert-Contains $officialTie 'output=n5[0] metric=4 scope_gates=4 filtered_gates=4 nets=7' 'official output filter contains n5[0]'
Assert-Contains $officialTie 'output=n5[1] metric=4 scope_gates=4 filtered_gates=4 nets=7' 'official output filter contains n5[1]'

Write-Host '[PASS] test49 cone CLI filter/detail regression complete'
