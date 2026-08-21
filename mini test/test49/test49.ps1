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

$orderedOptions = Invoke-Tools @(
    $read,
    'cone_query net_fanin y --with-pins --gate-types NAND NOR --with-paths'
)
Assert-Contains $orderedOptions 'status: ok' 'options accepted in mixed order'
Assert-Contains $orderedOptions 'filtered gates: 2' 'NAND/NOR union count'
Assert-Contains $orderedOptions 'longest local path depth:' 'path option preserved'

Write-Host '[PASS] test49 cone CLI filter/detail regression complete'
