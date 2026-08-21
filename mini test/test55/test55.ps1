$ErrorActionPreference = 'Stop'

# Direct Connectivity strict-parser regression (CONNECTIVITY-004).

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tools = Join-Path $root 'tools.exe'
$circuit = Join-Path $PSScriptRoot 'connectivity_strict.v'

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

# Zero is a valid threshold, and legacy conn_query shares the same parser.
$zero = Invoke-Tools @($read, 'structure_query fanout_violations 0')
Assert-Contains $zero 'status: ok' 'zero fanout limit is valid'

$legacyBad = Invoke-Tools @($read, 'conn_query gate_inputs g_and0 stray')
Assert-Contains $legacyBad 'status: error' 'legacy conn_query rejects trailing arg'
Assert-Contains $legacyBad 'Unexpected structure_query argument: stray' 'legacy parser diagnostic'
Assert-NotContains $legacyBad 'Unknown conn_query mode' 'legacy recognized mode is retained'

$unknown = Invoke-Tools @($read, 'structure_query definitely_not_a_mode')
Assert-Contains $unknown 'Unknown structure_query mode: definitely_not_a_mode' 'unknown mode remains distinct'

Write-Host '[PASS] CONNECTIVITY-004 strict parser regression completed'
