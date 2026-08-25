$ErrorActionPreference = 'Stop'

# Direct Connectivity parser and hidden-predicate regression.

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tools = Join-Path $root 'tools.exe'
$circuit = Join-Path $PSScriptRoot 'connectivity_strict.v'
$zeroFanoutCircuit = Join-Path $PSScriptRoot 'zero_fanout_pi.v'
$fanoutFilterCircuit = Join-Path $PSScriptRoot 'fanout_filter.v'
$fanoutRankingCircuit = Join-Path $PSScriptRoot 'fanout_ranking.v'

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

function Assert-Before([string] $Text, [string] $First, [string] $Second, [string] $Label) {
    $firstIndex = $Text.IndexOf($First)
    $secondIndex = $Text.IndexOf($Second)
    if ($firstIndex -lt 0 -or $secondIndex -lt 0 -or $firstIndex -ge $secondIndex) {
        throw "[$Label] Expected '$First' before '$Second'`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

$read = "read $circuit"

# Every public Direct Connectivity mode must retain one valid form.
$validCases = @(
    @{ Command = 'structure_query net_driver n1'; Label = 'net_driver' },
    @{ Command = 'structure_query net_driver n1 --with-pins'; Label = 'net_driver detail' },
    @{ Command = 'structure_query net_loads n1'; Label = 'net_loads' },
    @{ Command = 'structure_query net_loads n1 --with-pins'; Label = 'net_loads detail' },
    @{ Command = 'structure_query fanout_load n1'; Label = 'fanout_load' },
    @{ Command = 'structure_query fanout_report n1'; Label = 'fanout_report alias' },
    @{ Command = 'structure_query global_fanout'; Label = 'global_fanout omitted limit' },
    @{ Command = 'structure_query global_fanout 2'; Label = 'global_fanout limit' },
    @{ Command = 'structure_query pi_fanout'; Label = 'pi_fanout omitted limit' },
    @{ Command = 'structure_query pi_fanout 2'; Label = 'pi_fanout limit' },
    @{ Command = 'structure_query fanout_violations 2'; Label = 'fanout_violations' },
    @{ Command = 'structure_query fanout_filter pi eq 1'; Label = 'fanout_filter' },
    @{ Command = 'structure_query fanout_rank all highest'; Label = 'fanout_rank' },
    @{ Command = 'structure_query gate_inputs g_and0'; Label = 'gate_inputs' },
    @{ Command = 'structure_query gate_output g_and0'; Label = 'gate_output' },
    @{ Command = 'structure_query gate_fanin g_xor0'; Label = 'gate_fanin' },
    @{ Command = 'structure_query gate_fanout g_and0'; Label = 'gate_fanout' },
    @{ Command = 'structure_query is_connected g_and0 n1'; Label = 'is_connected' }
)

foreach ($case in $validCases) {
    $result = Invoke-Tools @($read, $case.Command)
    Assert-Contains $result 'status: ok' "$($case.Label) succeeds"
    Assert-Contains $result 'complete: true' "$($case.Label) is complete"
}

# All active candidates participate in extrema even when zero-fanout detail
# reports are omitted. Otherwise an all-zero PI scope has max=0 but no winners.
$allZeroPi = Invoke-Tools @(
    "read $zeroFanoutCircuit",
    'structure_query pi_fanout'
)
Assert-Contains $allZeroPi 'status: ok' 'all-zero PI fanout query succeeds'
Assert-Contains $allZeroPi 'complete: true' 'all-zero PI fanout query is complete'
Assert-Contains $allZeroPi 'checked nets: 2' 'all active PI candidates are checked'
Assert-Contains $allZeroPi 'max fanout: 0' 'all-zero PI maximum is zero'
Assert-Contains $allZeroPi 'Max-fanout nets (2)' 'all-zero PI ties are retained'
Assert-Contains $allZeroPi 'unused_a fanout=0' 'first zero-fanout PI is reported'
Assert-Contains $allZeroPi 'unused_b fanout=0' 'second zero-fanout PI is reported'

# Generalized fanout predicates use QA pin-level loads. The fixture has PI
# fanouts zero=0, one=1, two=2 (same gate, two pins), and three=3.
$filterRead = "read $fanoutFilterCircuit"
$filterCases = @(
    @{ Command = 'structure_query fanout_filter pi eq 0'; Count = 1; Names = @('zero fanout=0'); Label = 'equal zero' },
    @{ Command = 'structure_query fanout_filter pi eq 2'; Count = 1; Names = @('two fanout=2'); Label = 'equal two pin loads' },
    @{ Command = 'structure_query fanout_filter pi ne 0'; Count = 3; Names = @('one fanout=1', 'two fanout=2', 'three fanout=3'); Label = 'not equal zero' },
    @{ Command = 'structure_query fanout_filter pi gt 1'; Count = 2; Names = @('two fanout=2', 'three fanout=3'); Label = 'greater than' },
    @{ Command = 'structure_query fanout_filter pi ge 2'; Count = 2; Names = @('two fanout=2', 'three fanout=3'); Label = 'greater or equal' },
    @{ Command = 'structure_query fanout_filter pi lt 2'; Count = 2; Names = @('zero fanout=0', 'one fanout=1'); Label = 'less than' },
    @{ Command = 'structure_query fanout_filter pi le 1'; Count = 2; Names = @('zero fanout=0', 'one fanout=1'); Label = 'less or equal' },
    @{ Command = 'structure_query fanout_filter pi between 1 2'; Count = 2; Names = @('one fanout=1', 'two fanout=2'); Label = 'inclusive range' }
)

foreach ($case in $filterCases) {
    $result = Invoke-Tools @($filterRead, $case.Command)
    Assert-Contains $result 'status: ok' "$($case.Label) succeeds"
    Assert-Contains $result 'complete: true' "$($case.Label) is complete"
    Assert-Contains $result "matched net count: $($case.Count)" "$($case.Label) count"
    Assert-Contains $result "Matched nets ($($case.Count))" "$($case.Label) list size"
    foreach ($name in $case.Names) {
        Assert-Contains $result $name "$($case.Label) contains $name"
    }
}

$allScope = Invoke-Tools @($filterRead, 'structure_query fanout_filter all eq 1')
Assert-Contains $allScope 'matched net count: 4' 'all scope includes one PI and three PO nets'
Assert-Contains $allScope 'one fanout=1' 'all scope retains PI match'
Assert-Contains $allScope 'y_one fanout=1' 'all scope includes primary-output load'

$noFilterMatch = Invoke-Tools @($filterRead, 'structure_query fanout_filter pi eq 4')
Assert-Contains $noFilterMatch 'status: ok' 'fanout filter no-match succeeds'
Assert-Contains $noFilterMatch 'complete: true' 'fanout filter no-match is complete'
Assert-Contains $noFilterMatch 'matched net count: 0' 'fanout filter no-match count is zero'
Assert-Contains $noFilterMatch 'Matched nets (0)' 'fanout filter no-match list is explicit'

# Ranking uses distinct fanout values as ranks and retains every tie. PI fanout
# levels are 3:{high,tie_high}, 2:{mid}, 1:{clk,low,rst_n}, and 0:{zero}.
$rankingRead = "read $fanoutRankingCircuit"
$rankingCases = @(
    @{ Command = 'structure_query fanout_rank pi highest'; Count = 2; LevelCount = 1; Exists = 'yes'; Entries = @('rank=1 net=high fanout=3', 'rank=1 net=tie_high fanout=3'); Label = 'PI highest ties' },
    @{ Command = 'structure_query fanout_rank pi nth_highest 2'; Count = 1; LevelCount = 1; Exists = 'yes'; Entries = @('rank=2 net=mid fanout=2'); Label = 'PI second highest' },
    @{ Command = 'structure_query fanout_rank pi lowest'; Count = 1; LevelCount = 1; Exists = 'yes'; Entries = @('rank=1 net=zero fanout=0'); Label = 'PI lowest includes zero' },
    @{ Command = 'structure_query fanout_rank pi nth_lowest 2'; Count = 3; LevelCount = 1; Exists = 'yes'; Entries = @('rank=2 net=clk fanout=1', 'rank=2 net=low fanout=1', 'rank=2 net=rst_n fanout=1'); Label = 'PI second lowest ties' },
    @{ Command = 'structure_query fanout_rank pi top 2'; Count = 3; LevelCount = 2; Exists = 'yes'; Entries = @('rank=1 net=high fanout=3', 'rank=1 net=tie_high fanout=3', 'rank=2 net=mid fanout=2'); Label = 'PI top two levels' },
    @{ Command = 'structure_query fanout_rank pi bottom 2'; Count = 4; LevelCount = 2; Exists = 'yes'; Entries = @('rank=1 net=zero fanout=0', 'rank=2 net=clk fanout=1', 'rank=2 net=low fanout=1', 'rank=2 net=rst_n fanout=1'); Label = 'PI bottom two levels' },
    @{ Command = 'structure_query fanout_rank pi nth_highest 9'; Count = 0; LevelCount = 0; Exists = 'no'; Entries = @(); Label = 'missing PI rank' }
)

foreach ($case in $rankingCases) {
    $result = Invoke-Tools @($rankingRead, $case.Command)
    Assert-Contains $result 'status: ok' "$($case.Label) succeeds"
    Assert-Contains $result 'complete: true' "$($case.Label) is complete"
    Assert-Contains $result "result net count: $($case.Count)" "$($case.Label) result count"
    Assert-Contains $result "selected fanout levels: $($case.LevelCount)" "$($case.Label) selected levels"
    Assert-Contains $result "requested rank exists: $($case.Exists)" "$($case.Label) rank existence"
    Assert-Contains $result "Ranked nets ($($case.Count))" "$($case.Label) list size"
    foreach ($entry in $case.Entries) {
        Assert-Contains $result $entry "$($case.Label) contains $entry"
    }
}

$stableTies = Invoke-Tools @($rankingRead, 'structure_query fanout_rank pi highest')
Assert-Before $stableTies 'rank=1 net=high fanout=3' `
    'rank=1 net=tie_high fanout=3' 'same-rank names have stable lexical order'

$emptyRankScope = Invoke-Tools @($filterRead, 'structure_query fanout_rank dff_output highest')
Assert-Contains $emptyRankScope 'status: ok' 'empty ranking scope succeeds'
Assert-Contains $emptyRankScope 'checked nets: 0' 'empty ranking scope checks zero nets'
Assert-Contains $emptyRankScope 'result net count: 0' 'empty ranking scope has zero results'
Assert-Contains $emptyRankScope 'requested rank exists: no' 'empty ranking scope reports missing rank'

$rankingScopes = @(
    @{ Scope = 'po'; Checked = 3; Name = 'q'; Label = 'PO scope' },
    @{ Scope = 'internal'; Checked = 6; Name = 'n_high_a'; Label = 'internal scope' },
    @{ Scope = 'gate_output'; Checked = 9; Name = 'q'; Label = 'gate-output scope' },
    @{ Scope = 'comb_output'; Checked = 8; Name = 'y0'; Label = 'combinational-output scope' },
    @{ Scope = 'dff_output'; Checked = 1; Name = 'q'; Label = 'DFF-output scope' }
)
foreach ($case in $rankingScopes) {
    $result = Invoke-Tools @($rankingRead, "structure_query fanout_rank $($case.Scope) highest")
    Assert-Contains $result 'status: ok' "$($case.Label) succeeds"
    Assert-Contains $result "checked nets: $($case.Checked)" "$($case.Label) checked count"
    Assert-Contains $result "net=$($case.Name)" "$($case.Label) representative result"
}

# DirectlyConnected accepts a scalar net or a bus base name. For a bus query,
# count/net names describe the active bits that are actually connected.
$descendingBus = Invoke-Tools @($read, 'structure_query is_connected g_bus_multi data_in')
Assert-Contains $descendingBus 'status: ok' 'descending bus query succeeds'
Assert-Contains $descendingBus 'connected: yes' 'descending bus detects a connection'
Assert-Contains $descendingBus 'count: 2' 'descending bus counts connected bits'
Assert-Contains $descendingBus 'data_in[0]' 'descending bus reports connected bit zero'
Assert-Contains $descendingBus 'data_in[2]' 'descending bus reports connected bit two'

$ascendingBus = Invoke-Tools @($read, 'structure_query is_connected g_bus_ascending ascending_in')
Assert-Contains $ascendingBus 'status: ok' 'ascending bus query succeeds'
Assert-Contains $ascendingBus 'connected: yes' 'ascending bus detects a connection'
Assert-Contains $ascendingBus 'count: 1' 'ascending bus counts one connected bit'
Assert-Contains $ascendingBus 'ascending_in[2]' 'ascending bus reports the exact connected bit'

$outputBus = Invoke-Tools @($read, 'structure_query is_connected g_bus_output data_out')
Assert-Contains $outputBus 'connected: yes' 'bus query includes a gate output connection'
Assert-Contains $outputBus 'data_out[1]' 'bus output query reports the driven bit'

$disconnectedBus = Invoke-Tools @($read, 'structure_query is_connected g_buf0 data_in')
Assert-Contains $disconnectedBus 'status: ok' 'existing disconnected bus is a valid query'
Assert-Contains $disconnectedBus 'connected: no' 'existing disconnected bus reports no'
Assert-Contains $disconnectedBus 'count: 0' 'existing disconnected bus reports zero bits'

$scalarConnection = Invoke-Tools @($read, 'structure_query is_connected g_bus_multi data_in[0]')
Assert-Contains $scalarConnection 'connected: yes' 'scalar bit behavior remains supported'
Assert-Contains $scalarConnection 'count: 1' 'scalar bit count remains one'

$missingBus = Invoke-Tools @($read, 'structure_query is_connected g_bus_multi missing_bus')
Assert-Contains $missingBus 'status: error' 'missing bus name is rejected'
Assert-Contains $missingBus 'complete: false' 'missing bus query is incomplete'
Assert-Contains $missingBus 'Net not found: missing_bus' 'missing bus diagnostic is explicit'

$afterCleanupGate = Invoke-Tools @(
    $read,
    'edit_apply remove_dangling_logic',
    'structure_query is_connected g_bus_multi data_in'
)
Assert-Contains $afterCleanupGate 'Gate not found: g_bus_multi' 'removed gate tombstone is rejected'

$afterCleanupNet = Invoke-Tools @(
    $read,
    'edit_apply remove_dangling_logic',
    'structure_query is_connected g_and0 n_bus'
)
Assert-Contains $afterCleanupNet 'Net not found: n_bus' 'removed net tombstone is rejected'

# Required names and exact arity.
$arityCases = @(
    @{ Command = 'structure_query net_driver'; Error = 'net_driver requires <net>'; Label = 'net_driver missing net' },
    @{ Command = 'structure_query net_loads --with-pins'; Error = 'net_loads requires <net>'; Label = 'net_loads missing net' },
    @{ Command = 'structure_query fanout_load'; Error = 'fanout_load requires <net>'; Label = 'fanout_load missing net' },
    @{ Command = 'structure_query fanout_report n1 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'fanout_report trailing arg' },
    @{ Command = 'structure_query gate_inputs'; Error = 'gate_inputs requires <gate>'; Label = 'gate_inputs missing gate' },
    @{ Command = 'structure_query gate_output g_and0 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'gate_output trailing arg' },
    @{ Command = 'structure_query gate_fanin --bad'; Error = 'gate_fanin requires <gate>'; Label = 'gate_fanin option instead of gate' },
    @{ Command = 'structure_query gate_fanout g_and0 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'gate_fanout trailing arg' },
    @{ Command = 'structure_query is_connected g_and0'; Error = 'is_connected requires <gate> <net>'; Label = 'is_connected missing net' },
    @{ Command = 'structure_query is_connected g_and0 n1 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'is_connected trailing arg' }
)

foreach ($case in $arityCases) {
    $result = Invoke-Tools @($read, $case.Command)
    Assert-Contains $result 'status: error' "$($case.Label) rejected"
    Assert-Contains $result 'complete: false' "$($case.Label) is incomplete"
    Assert-Contains $result $case.Error "$($case.Label) diagnostic"
    Assert-NotContains $result 'Unknown structure_query mode' "$($case.Label) keeps recognized mode"
}

# --with-pins is the only optional token on net_driver/net_loads.
$pinOptionCases = @(
    @{ Command = 'structure_query net_driver n1 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'net_driver stray arg' },
    @{ Command = 'structure_query net_loads n1 --mystery'; Error = 'Unknown structure_query option: --mystery'; Label = 'net_loads unknown option' },
    @{ Command = 'structure_query net_driver n1 --with-pins --with-pins'; Error = 'Unexpected structure_query argument: --with-pins'; Label = 'duplicate with-pins' }
)

foreach ($case in $pinOptionCases) {
    $result = Invoke-Tools @($read, $case.Command)
    Assert-Contains $result 'status: error' "$($case.Label) rejected"
    Assert-Contains $result $case.Error "$($case.Label) diagnostic"
    Assert-NotContains $result 'Unknown structure_query mode' "$($case.Label) keeps recognized mode"
}

# Fanout limits are strict, non-negative, base-10 integers.
$limitCases = @(
    @{ Command = 'structure_query global_fanout abc'; Error = 'global_fanout accepts only a non-negative integer fanout limit'; Label = 'global text limit' },
    @{ Command = 'structure_query global_fanout -1'; Error = 'global_fanout accepts only a non-negative integer fanout limit'; Label = 'global negative limit' },
    @{ Command = 'structure_query global_fanout 1.5'; Error = 'global_fanout accepts only a non-negative integer fanout limit'; Label = 'global decimal limit' },
    @{ Command = 'structure_query global_fanout 2147483648'; Error = 'global_fanout accepts only a non-negative integer fanout limit'; Label = 'global overflow limit' },
    @{ Command = 'structure_query global_fanout 2 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'global trailing arg' },
    @{ Command = 'structure_query pi_fanout abc'; Error = 'pi_fanout accepts only a non-negative integer fanout limit'; Label = 'PI text limit' },
    @{ Command = 'structure_query fanout_violations'; Error = 'fanout_violations requires a non-negative integer <limit>'; Label = 'violations missing limit' },
    @{ Command = 'structure_query fanout_violations -1'; Error = 'fanout_violations requires a non-negative integer <limit>'; Label = 'violations negative limit' },
    @{ Command = 'structure_query fanout_violations 2 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'violations trailing arg' }
)

foreach ($case in $limitCases) {
    $result = Invoke-Tools @($read, $case.Command)
    Assert-Contains $result 'status: error' "$($case.Label) rejected"
    Assert-Contains $result 'complete: false' "$($case.Label) is incomplete"
    Assert-Contains $result $case.Error "$($case.Label) diagnostic"
    Assert-NotContains $result 'Unknown structure_query mode' "$($case.Label) keeps recognized mode"
}

# fanout_filter has a strict scope/predicate/bound grammar.
$filterErrorCases = @(
    @{ Command = 'structure_query fanout_filter'; Error = 'fanout_filter requires <all|pi>'; Label = 'filter missing arguments' },
    @{ Command = 'structure_query fanout_filter internal eq 1'; Error = 'fanout_filter scope must be all or pi'; Label = 'filter invalid scope' },
    @{ Command = 'structure_query fanout_filter pi approximately 1'; Error = 'fanout_filter predicate must be'; Label = 'filter invalid predicate' },
    @{ Command = 'structure_query fanout_filter pi eq -1'; Error = 'fanout_filter value must be a non-negative integer'; Label = 'filter negative value' },
    @{ Command = 'structure_query fanout_filter pi eq 1.5'; Error = 'fanout_filter value must be a non-negative integer'; Label = 'filter decimal value' },
    @{ Command = 'structure_query fanout_filter pi eq 18446744073709551616'; Error = 'fanout_filter value must be a non-negative integer'; Label = 'filter overflow value' },
    @{ Command = 'structure_query fanout_filter pi between 1'; Error = 'fanout_filter between requires a non-negative integer <upper>'; Label = 'filter missing upper' },
    @{ Command = 'structure_query fanout_filter pi between 3 2'; Error = 'fanout_filter lower bound must not exceed upper bound'; Label = 'filter reversed range' },
    @{ Command = 'structure_query fanout_filter pi eq 1 2'; Error = 'Unexpected structure_query argument: 2'; Label = 'filter unexpected upper' },
    @{ Command = 'structure_query fanout_filter pi between 1 2 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'filter trailing argument' }
)

foreach ($case in $filterErrorCases) {
    $result = Invoke-Tools @($filterRead, $case.Command)
    Assert-Contains $result 'status: error' "$($case.Label) rejected"
    Assert-Contains $result 'complete: false' "$($case.Label) is incomplete"
    Assert-Contains $result $case.Error "$($case.Label) diagnostic"
    Assert-NotContains $result 'Unknown structure_query mode' "$($case.Label) keeps recognized mode"
}

# fanout_rank has strict scope/mode/k grammar. K counts distinct fanout levels.
$rankingErrorCases = @(
    @{ Command = 'structure_query fanout_rank'; Error = 'fanout_rank requires <scope> <mode> [k]'; Label = 'rank missing arguments' },
    @{ Command = 'structure_query fanout_rank bus highest'; Error = 'fanout_rank scope must be'; Label = 'rank invalid scope' },
    @{ Command = 'structure_query fanout_rank pi median'; Error = 'fanout_rank mode must be'; Label = 'rank invalid mode' },
    @{ Command = 'structure_query fanout_rank pi top'; Error = 'fanout_rank top requires a positive integer <k>'; Label = 'rank missing k' },
    @{ Command = 'structure_query fanout_rank pi top 0'; Error = 'fanout_rank top requires a positive integer <k>'; Label = 'rank zero k' },
    @{ Command = 'structure_query fanout_rank pi top -1'; Error = 'fanout_rank top requires a positive integer <k>'; Label = 'rank negative k' },
    @{ Command = 'structure_query fanout_rank pi top 1.5'; Error = 'fanout_rank top requires a positive integer <k>'; Label = 'rank decimal k' },
    @{ Command = 'structure_query fanout_rank pi top 18446744073709551616'; Error = 'fanout_rank top requires a positive integer <k>'; Label = 'rank overflow k' },
    @{ Command = 'structure_query fanout_rank pi highest 2'; Error = 'Unexpected structure_query argument: 2'; Label = 'rank unexpected k' },
    @{ Command = 'structure_query fanout_rank pi bottom 2 stray'; Error = 'Unexpected structure_query argument: stray'; Label = 'rank trailing argument' }
)

foreach ($case in $rankingErrorCases) {
    $result = Invoke-Tools @($rankingRead, $case.Command)
    Assert-Contains $result 'status: error' "$($case.Label) rejected"
    Assert-Contains $result 'complete: false' "$($case.Label) is incomplete"
    Assert-Contains $result $case.Error "$($case.Label) diagnostic"
    Assert-NotContains $result 'Unknown structure_query mode' "$($case.Label) keeps recognized mode"
}

# Zero is a valid threshold, and legacy conn_query shares the same parser.
$zero = Invoke-Tools @($read, 'structure_query fanout_violations 0')
Assert-Contains $zero 'status: ok' 'zero fanout limit is valid'

$legacyBad = Invoke-Tools @($read, 'conn_query gate_inputs g_and0 stray')
Assert-Contains $legacyBad 'status: error' 'legacy conn_query rejects trailing arg'
Assert-Contains $legacyBad 'Unexpected structure_query argument: stray' 'legacy parser diagnostic'
Assert-NotContains $legacyBad 'Unknown conn_query mode' 'legacy recognized mode is retained'

$unknown = Invoke-Tools @($read, 'structure_query definitely_not_a_mode')
Assert-Contains $unknown 'Unknown structure_query mode: definitely_not_a_mode' 'unknown mode remains distinct'

Write-Host '[PASS] Direct Connectivity regression completed'
