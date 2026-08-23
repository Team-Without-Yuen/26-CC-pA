$ErrorActionPreference = 'Stop'

# structure_query gate-type filter regression (BASIC-001)
# 覆蓋：多型別 OR 聯集、未指定=全部、legacy positional 相容、valid zero、
#       count_by_type 無 filter 的 9 型別行為、以及所有錯誤訊息診斷。

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tools = Join-Path $root 'tools.exe'
$circuit = Join-Path $PSScriptRoot 'basic_filter.v'

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

# ---------------------------------------------------------------------------
# fixture 基準：AND=3 OR=1 NAND=3 NOR=2 NOT=2 BUF=1 XOR=2 XNOR=1 DFF=2，共 17 顆
# 其中 g_dead 是不驅動任何輸出的 dangling XOR，供 tombstone 檢查使用
# ---------------------------------------------------------------------------

$summary = Invoke-Tools @($read, 'structure_query summary')
Assert-Contains $summary 'status: ok' 'summary succeeds'
Assert-Contains $summary '  gates: 17' 'fixture has 17 active gates'
Assert-Contains $summary '  primary inputs: 6' 'summary preserves PI port count'
Assert-Contains $summary '  primary input bits: 7' 'summary reports aggregate PI bit count'
Assert-Contains $summary '  primary outputs: 3' 'summary preserves PO port count'
Assert-Contains $summary '  primary output bits: 4' 'summary reports aggregate PO bit count'

# ---------------------------------------------------------------------------
# Net classification：一次掃描回傳獨立 PI/PO/constant flags 與 exclusive internal
# fixture：PI bits=7、PO bits=4、constants=2、internal=13、PI+PO overlap=0
# ---------------------------------------------------------------------------

$netClasses = Invoke-Tools @($read, 'structure_query net_classes')
Assert-Contains $netClasses 'status: ok' 'net_classes succeeds'
Assert-Contains $netClasses 'complete: true' 'net_classes is complete'
Assert-Contains $netClasses '  active nets: 26' 'net_classes counts every active net once'
Assert-Contains $netClasses '  primary-input nets: 7' 'net_classes counts PI bit nets'
Assert-Contains $netClasses '  primary-output nets: 4' 'net_classes counts PO bit nets'
Assert-Contains $netClasses '  primary-input/output nets: 0' 'net_classes reports a valid empty overlap'
Assert-Contains $netClasses '  constant nets: 2' 'net_classes counts parser constants'
Assert-Contains $netClasses '  internal nets: 13' 'net_classes counts the exclusive internal remainder'
Assert-Contains $netClasses "Constant nets (2):" 'net_classes prints a self-described constant section'
Assert-Contains $netClasses "  1'b0" 'net_classes lists constant zero'
Assert-Contains $netClasses "  1'b1" 'net_classes lists constant one'
Assert-Contains $netClasses 'Internal nets (13):' 'net_classes prints the complete internal section'
Assert-Contains $netClasses '  n_dead' 'net_classes includes an active dangling internal net'

# ---------------------------------------------------------------------------
# 單一型別 vs 多型別聯集
# ---------------------------------------------------------------------------

$and = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types AND')
Assert-Contains $and 'filtered gates: 3' 'AND count'
Assert-Contains $and 'scope gates: 17' 'scope stays unfiltered'
Assert-Contains $and 'gate type filter applied: yes' 'filter flag set'

$or = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types OR')
Assert-Contains $or 'filtered gates: 1' 'OR count'

$andOr = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types AND OR')
Assert-Contains $andOr 'filtered gates: 4' 'AND|OR union equals 3 + 1'
Assert-Contains $andOr 'gate type filters: AND, OR' 'filters reported in enum order'
Assert-Contains $andOr 'g_and0' 'union keeps AND members'
Assert-Contains $andOr 'g_or0' 'union keeps OR members'

$nandNor = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types NAND NOR')
Assert-Contains $nandNor 'filtered gates: 5' 'NAND|NOR union equals 3 + 2'

$dedup = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types nand NOR nand')
Assert-Contains $dedup 'filtered gates: 5' 'duplicate type does not double count'
Assert-Contains $dedup 'gate type filters: NAND, NOR' 'case-insensitive and deduplicated'

# ---------------------------------------------------------------------------
# exclude filter：無 include 表示從全部 active gates 扣除；同時指定時 exclude 優先
# ---------------------------------------------------------------------------

$withoutDff = Invoke-Tools @($read,
    'structure_query gates_by_type --exclude-gate-types DFF')
Assert-Contains $withoutDff 'filtered gates: 15' 'exclude-only filter starts from all active gates'
Assert-Contains $withoutDff 'gate type filters: all' 'exclude-only include scope is all'
Assert-Contains $withoutDff 'gate type exclusion applied: yes' 'exclude flag set'
Assert-Contains $withoutDff 'excluded gate type filters: DFF' 'excluded type reported'
Assert-NotContains $withoutDff 'g_dff0' 'excluded DFF members are absent'

$includeExclude = Invoke-Tools @($read,
    'structure_query gates_by_type --gate-types AND OR NOT --exclude-gate-types OR')
Assert-Contains $includeExclude 'filtered gates: 5' 'exclude is applied after include union'
Assert-Contains $includeExclude 'gate type filters: AND, OR, NOT' 'include set reported'
Assert-Contains $includeExclude 'excluded gate type filters: OR' 'overlapping exclusion reported'
Assert-NotContains $includeExclude 'g_or0' 'exclude takes precedence over include'

$excludedToZero = Invoke-Tools @($read,
    'structure_query count_by_type --gate-types DFF --exclude-gate-types DFF')
Assert-Contains $excludedToZero 'status: ok' 'include/exclude overlap is a valid query'
Assert-Contains $excludedToZero 'filtered gates: 0' 'exclude precedence can produce valid zero'

$constExclude = Invoke-Tools @($read,
    'structure_query const_input_gates --exclude-gate-types AND')
Assert-Contains $constExclude 'scope gates: 5' 'constant-input exclusion preserves candidate scope'
Assert-Contains $constExclude 'filtered gates: 4' 'constant-input exclusion removes matching type'

# ---------------------------------------------------------------------------
# 未指定型別 = 全部（BASIC-001 之前無法表達）
# ---------------------------------------------------------------------------

$allGates = Invoke-Tools @($read, 'structure_query gates_by_type')
Assert-Contains $allGates 'status: ok' 'omitted gate type is accepted'
Assert-Contains $allGates '  gates: 17' 'unfiltered list returns every gate'

$allPins = Invoke-Tools @($read, 'structure_query gates_by_type --with-pins')
Assert-Contains $allPins 'status: ok' 'unfiltered detail query succeeds'
Assert-Contains $allPins 'gate detail count: 17' 'every gate has a pin/net record'
Assert-Contains $allPins 'gate=g_dff0 type=DFF' 'DFF pin detail present in unfiltered list'

$allTypes = Invoke-Tools @($read,
    'structure_query gates_by_type --gate-types AND OR NOT NAND NOR XOR XNOR BUF DFF')
Assert-Contains $allTypes 'filtered gates: 17' 'explicit all-type list equals unfiltered total'

# ---------------------------------------------------------------------------
# legacy positional 形式必須維持可用
# ---------------------------------------------------------------------------

$legacy = Invoke-Tools @($read, 'structure_query gates_by_type NAND')
Assert-Contains $legacy 'filtered gates: 3' 'positional single type still works'

$legacyPins = Invoke-Tools @($read, 'structure_query gates_by_type NAND --with-pins')
Assert-Contains $legacyPins 'gate detail count: 3' 'positional type with --with-pins still works'

# ---------------------------------------------------------------------------
# const_input_gates
# ---------------------------------------------------------------------------

$constAny = Invoke-Tools @($read, 'structure_query const_input_gates')
Assert-Contains $constAny '  gates: 5' 'five gates have a constant input'

$const0 = Invoke-Tools @($read, 'structure_query const_input_gates all 0')
Assert-Contains $const0 '  gates: 2' 'two gates have a constant 0 input'

$const1 = Invoke-Tools @($read, 'structure_query const_input_gates all 1')
Assert-Contains $const1 '  gates: 3' 'three gates have a constant 1 input'

$constNand = Invoke-Tools @($read, 'structure_query const_input_gates --gate-types NAND')
Assert-Contains $constNand 'filtered gates: 1' 'constant-input NAND count'
Assert-Contains $constNand 'scope gates: 5' 'scope is the unfiltered constant-input set'

$constNandNor = Invoke-Tools @($read, 'structure_query const_input_gates --gate-types NAND NOR')
Assert-Contains $constNandNor 'filtered gates: 2' 'constant-input NAND|NOR union'

$constNamed = Invoke-Tools @($read,
    'structure_query const_input_gates --gate-types NAND NOR --const 0')
Assert-Contains $constNamed 'filtered gates: 1' '--const applies after --gate-types'

$constMixed = Invoke-Tools @($read, 'structure_query const_input_gates all 0 --gate-types NOR')
Assert-Contains $constMixed 'filtered gates: 1' 'all + positional constant + --gate-types'

$constZero = Invoke-Tools @($read, 'structure_query const_input_gates --gate-types XOR')
Assert-Contains $constZero 'status: ok' 'no match is not an error'
Assert-Contains $constZero 'filtered gates: 0' 'valid zero result'
Assert-Contains $constZero 'complete: true' 'valid zero is complete'

# ---------------------------------------------------------------------------
# count_by_type：無 filter 路徑是每個官方 testcase 第 3 行都會走的，必須維持原行為
# ---------------------------------------------------------------------------

$countAll = Invoke-Tools @($read, 'structure_query count_by_type')
Assert-Contains $countAll '  gates: 17' 'unfiltered count total'
foreach ($t in @('AND : 3','OR : 1','NAND : 3','NOR : 2','NOT : 2','BUF : 1','XOR : 2','XNOR : 1','DFF : 2')) {
    Assert-Contains $countAll $t "unfiltered breakdown lists $t"
}
Assert-NotContains $countAll 'gate type filter applied' 'unfiltered count omits filter block'

$countTwo = Invoke-Tools @($read, 'structure_query count_by_type --gate-types AND OR')
Assert-Contains $countTwo '  gates: 4' 'filtered count sums the requested types'
Assert-Contains $countTwo 'AND : 3' 'filtered breakdown keeps AND'
Assert-Contains $countTwo 'OR : 1' 'filtered breakdown keeps OR'
Assert-NotContains $countTwo 'NAND : ' 'filtered breakdown omits other types'

$countLegacy = Invoke-Tools @($read, 'structure_query count_by_type NAND')
Assert-Contains $countLegacy '  gates: 3' 'positional count_by_type still works'

# ---------------------------------------------------------------------------
# 錯誤訊息診斷
# ---------------------------------------------------------------------------

$badType = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types MUX')
Assert-Contains $badType 'status: error' 'unsupported type rejected'
Assert-Contains $badType "Unsupported gate type 'MUX'" 'unsupported type diagnostic'
Assert-Contains $badType 'AND OR NOT NAND NOR XOR XNOR BUF DFF' 'supported type list reported'

$badPositional = Invoke-Tools @($read, 'structure_query gates_by_type MUX')
Assert-Contains $badPositional "Unsupported gate type 'MUX'" 'positional bad type diagnosed'

$emptyTypes = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types')
Assert-Contains $emptyTypes 'status: error' 'empty type list rejected'
Assert-Contains $emptyTypes '--gate-types requires at least one gate type' 'empty type diagnostic'

$unknownOption = Invoke-Tools @($read, 'structure_query gates_by_type --mystery')
Assert-Contains $unknownOption 'Unknown structure_query option: --mystery' 'unknown option diagnostic'

$mixedType = Invoke-Tools @($read, 'structure_query gates_by_type NAND --gate-types NOR')
Assert-Contains $mixedType 'Cannot combine a positional gate type with --gate-types' 'positional + option conflict'

$dupPins = Invoke-Tools @($read, 'structure_query gates_by_type NAND --with-pins --with-pins')
Assert-Contains $dupPins 'Duplicate structure_query option: --with-pins' 'duplicate --with-pins diagnostic'

$dupTypes = Invoke-Tools @($read, 'structure_query gates_by_type --gate-types AND --gate-types OR')
Assert-Contains $dupTypes 'Duplicate structure_query option: --gate-types' 'duplicate --gate-types diagnostic'

$emptyExcludedTypes = Invoke-Tools @($read,
    'structure_query gates_by_type --exclude-gate-types --with-pins')
Assert-Contains $emptyExcludedTypes 'status: error' 'empty excluded type list rejected'
Assert-Contains $emptyExcludedTypes '--exclude-gate-types requires at least one gate type' 'empty exclusion diagnostic'

$dupExcludedTypes = Invoke-Tools @($read,
    'structure_query gates_by_type --exclude-gate-types AND --exclude-gate-types OR')
Assert-Contains $dupExcludedTypes 'Duplicate structure_query option: --exclude-gate-types' 'duplicate exclusion diagnostic'

$countPins = Invoke-Tools @($read, 'structure_query count_by_type --with-pins')
Assert-Contains $countPins 'count_by_type does not support --with-pins' 'inapplicable option diagnostic'

# ---------------------------------------------------------------------------
# 固定 arity modes：不得忽略缺少值或 trailing token
# ---------------------------------------------------------------------------

foreach ($mode in @('summary', 'list_gates', 'list_nets', 'list_pi', 'list_po',
                     'net_classes', 'list_dffs', 'list_comb', 'list_comb_gates',
                     'structural_issues')) {
    $result = Invoke-Tools @($read, "structure_query $mode stray")
    Assert-Contains $result 'status: error' "$mode rejects trailing argument"
    Assert-Contains $result 'complete: false' "$mode trailing argument is incomplete"
    Assert-Contains $result "$mode does not accept arguments" "$mode gives an arity diagnostic"
    Assert-NotContains $result 'Unknown structure_query mode' "$mode error keeps recognized mode"
}

$validGateInfo = Invoke-Tools @($read, 'structure_query gate_info g_and0')
Assert-Contains $validGateInfo 'status: ok' 'gate_info exact arity remains valid'
Assert-Contains $validGateInfo 'gate details included: yes' 'gate_info reports structured detail metadata'
Assert-Contains $validGateInfo 'Gate connection details (1):' 'gate_info prints one structured record'
Assert-Contains $validGateInfo 'gate=g_and0 type=AND inputs=[' 'gate_info structured record includes type and inputs'
Assert-Contains $validGateInfo 'IN1=a(PI)' 'gate_info marks primary-input connections'
Assert-NotContains $validGateInfo 'Inputs:' 'gate_info suppresses duplicate legacy pin text'

$constantGateInfo = Invoke-Tools @($read, 'structure_query gate_info g_nand1')
Assert-Contains $constantGateInfo "IN2=1'b1" 'gate_info preserves constant-input metadata'

$outputGateInfo = Invoke-Tools @($read, 'structure_query gate_info g_buf0')
Assert-Contains $outputGateInfo 'output=OUT=y0(PO)' 'gate_info marks primary-output connections'

$dffGateInfo = Invoke-Tools @($read, 'structure_query gate_info g_dff0')
Assert-Contains $dffGateInfo 'gate=g_dff0 type=DFF inputs=[' 'gate_info identifies the DFF record'
Assert-Contains $dffGateInfo 'RN=rst_n(PI)' 'gate_info preserves the DFF reset pin'
Assert-Contains $dffGateInfo "SN=1'b1" 'gate_info preserves the DFF set constant'
Assert-Contains $dffGateInfo 'CK=clk(PI)' 'gate_info preserves the DFF clock pin'
Assert-Contains $dffGateInfo 'D=y0(PO)' 'gate_info preserves the DFF data pin'
Assert-Contains $dffGateInfo 'output=Q=q[0](PO)' 'gate_info preserves the DFF output pin'
$validNetInfo = Invoke-Tools @($read, 'structure_query net_info n1')
Assert-Contains $validNetInfo 'status: ok' 'net_info exact arity remains valid'
$validPortInfo = Invoke-Tools @($read, 'structure_query port_info a')
Assert-Contains $validPortInfo 'status: ok' 'port_info exact arity remains valid'

foreach ($case in @(
    @{ Command = 'structure_query gate_info'; Missing = 'gate_info requires <gate>'; Label = 'gate_info missing value' },
    @{ Command = 'structure_query net_info'; Missing = 'net_info requires <net>'; Label = 'net_info missing value' },
    @{ Command = 'structure_query port_info'; Missing = 'port_info requires <port>'; Label = 'port_info missing value' },
    @{ Command = 'structure_query gate_info g_and0 stray'; Missing = 'Unexpected structure_query argument: stray'; Label = 'gate_info trailing value' },
    @{ Command = 'structure_query net_info n1 stray'; Missing = 'Unexpected structure_query argument: stray'; Label = 'net_info trailing value' },
    @{ Command = 'structure_query port_info a stray'; Missing = 'Unexpected structure_query argument: stray'; Label = 'port_info trailing value' }
)) {
    $result = Invoke-Tools @($read, $case.Command)
    Assert-Contains $result 'status: error' "$($case.Label) is rejected"
    Assert-Contains $result 'complete: false' "$($case.Label) is incomplete"
    Assert-Contains $result $case.Missing "$($case.Label) diagnostic"
    Assert-NotContains $result 'Unknown structure_query mode' "$($case.Label) keeps recognized mode"
}

# 只有 mode 本身不存在時才可以回 mode 訊息
$unknownMode = Invoke-Tools @($read, 'structure_query definitely_not_a_mode')
Assert-Contains $unknownMode 'Unknown structure_query mode: definitely_not_a_mode' 'unknown mode still diagnosed'

# 這是 BASIC-001 的核心迴歸鎖：參數錯誤絕不可再被回報成 mode 問題
foreach ($case in @($badType, $badPositional, $emptyTypes, $unknownOption,
                    $mixedType, $dupPins, $dupTypes, $emptyExcludedTypes,
                    $dupExcludedTypes, $countPins)) {
    Assert-NotContains $case 'Unknown structure_query mode' 'argument errors never blame the mode'
}

# ---------------------------------------------------------------------------
# tombstone：edit 後被移除的 gate 不得出現在任何欄位
# g_dead 是唯一不驅動任何輸出的 gate，cleanup 後 XOR 應從 2 降為 1
# ---------------------------------------------------------------------------

$beforeEdit = Invoke-Tools @($read, 'structure_query count_by_type --gate-types XOR')
Assert-Contains $beforeEdit 'filtered gates: 2' 'dangling XOR present before cleanup'

$afterEdit = Invoke-Tools @(
    $read,
    'edit_apply remove_dangling_logic',
    'structure_query count_by_type --gate-types XOR',
    'structure_query gates_by_type --gate-types XOR --with-pins'
)
Assert-Contains $afterEdit 'filtered gates: 1' 'dangling XOR removed from filtered count'
Assert-Contains $afterEdit 'gate detail count: 1' 'tombstone excluded from pin details'
Assert-NotContains $afterEdit 'g_dead' 'tombstone name never surfaces'

$afterEditAll = Invoke-Tools @(
    $read,
    'edit_apply remove_dangling_logic',
    'structure_query gates_by_type --with-pins'
)
Assert-NotContains $afterEditAll 'g_dead' 'unfiltered list excludes the removed gate'
Assert-NotContains $afterEditAll 'UNKNOWN' 'tombstone type never surfaces in an unfiltered list'

$afterEditNetClasses = Invoke-Tools @(
    $read,
    'edit_apply remove_dangling_logic',
    'structure_query net_classes'
)
Assert-Contains $afterEditNetClasses '  active nets: 25' 'removed net tombstone is excluded from active count'
Assert-Contains $afterEditNetClasses '  internal nets: 12' 'removed internal net is excluded from class count'
Assert-NotContains $afterEditNetClasses '  n_dead' 'removed net name never surfaces in class lists'

# ---------------------------------------------------------------------------
# structural_issues pin-level report：只列 named netlist 中實際存在的空 pin slot
# ---------------------------------------------------------------------------

$connectedIssues = Invoke-Tools @($read, 'structure_query structural_issues')
Assert-Contains $connectedIssues '  unconnected gates: 0' 'connected fixture reports a valid zero gate count'
Assert-Contains $connectedIssues '  unconnected input pins: 0' 'connected fixture reports a valid zero input-pin count'
Assert-Contains $connectedIssues '  unconnected output pins: 0' 'connected fixture reports a valid zero output-pin count'

$pinCircuit = Join-Path $PSScriptRoot 'unconnected_pins.v'
$pinIssues = Invoke-Tools @(
    "read $pinCircuit",
    'structure_query structural_issues'
)
Assert-Contains $pinIssues 'status: ok' 'unconnected pin query succeeds'
Assert-Contains $pinIssues 'complete: true' 'unconnected pin query is complete'
Assert-Contains $pinIssues '  unconnected gates: 3' 'three gates contain explicit unconnected slots'
Assert-Contains $pinIssues '  unconnected input pins: 2' 'input-hole and explicit RN hole are counted'
Assert-Contains $pinIssues '  unconnected output pins: 1' 'output hole is counted'
Assert-Contains $pinIssues 'Unconnected pin details (3):' 'pin-level details are returned'
Assert-Contains $pinIssues 'gate=g_missing_input type=AND direction=input pin=IN2 pin_index=1 net_id=-1 net=<unconnected> reason=UNCONNECTED' 'positional input hole is self-described'
Assert-Contains $pinIssues 'gate=g_missing_output type=OR direction=output pin=OUT pin_index=0 net_id=-1 net=<unconnected> reason=UNCONNECTED' 'positional output hole is self-described'
Assert-Contains $pinIssues 'gate=ff_open_rn type=DFF direction=input pin=RN pin_index=2 net_id=-1 net=<unconnected> reason=UNCONNECTED' 'explicit empty DFF control keeps its named pin'
Assert-NotContains $pinIssues 'gate=g_ok type=BUF direction=' 'fully connected gate is absent from pin issues'

# 大型 pin issue 必須依 4096-token policy 自動改寫完整 artifact。
$largeCircuit = Join-Path $PSScriptRoot 'generated_unconnected_large.v'
$largeLines = [System.Collections.Generic.List[string]]::new()
$largeLines.Add('module generated_unconnected_large(input a, output y);')
for ($i = 0; $i -lt 150; ++$i) {
    $largeLines.Add("wire n$i;")
    $largeLines.Add("and g_hole_$i(n$i, a, );")
}
$largeLines.Add('buf g_output(y, a);')
$largeLines.Add('endmodule')
[System.IO.File]::WriteAllLines($largeCircuit, $largeLines)

try {
    $largeIssues = Invoke-Tools @(
        "read $largeCircuit",
        'structure_query structural_issues'
    )
    Assert-Contains $largeIssues '  unconnected gates: 150' 'large query preserves exact gate count'
    Assert-Contains $largeIssues '  unconnected input pins: 150' 'large query preserves exact pin count'
    Assert-Contains $largeIssues 'list artifact complete: yes' 'large pin report uses a complete artifact'
    Assert-Contains $largeIssues 'response token limit: 4096' 'large pin report uses the official token limit'
    Assert-Contains $largeIssues 'artifact triggered by token estimate: yes' 'large pin report records the trigger'
    Assert-NotContains $largeIssues 'Unconnected pin details (150):' 'large pin records are suppressed from terminal'

    $artifactMatch = [regex]::Match($largeIssues, '(?m)^  output_file: (.+)$')
    if (-not $artifactMatch.Success) {
        throw "[large pin artifact path] Missing output_file`n--- output ---`n$largeIssues"
    }
    $artifactPath = Join-Path $root $artifactMatch.Groups[1].Value.Trim()
    if (-not (Test-Path -LiteralPath $artifactPath)) {
        throw "[large pin artifact file] File does not exist: $artifactPath"
    }
    $artifactText = Get-Content -LiteralPath $artifactPath -Raw
    Assert-Contains $artifactText 'Unconnected pin details (150):' 'artifact contains every pin record'
    Assert-Contains $artifactText 'gate=g_hole_0 type=AND direction=input pin=IN2' 'artifact contains the first pin record'
    Assert-Contains $artifactText 'gate=g_hole_149 type=AND direction=input pin=IN2' 'artifact contains the last pin record'
    Assert-Contains $artifactText 'Complete: yes' 'artifact footer declares completeness'
    Remove-Item -LiteralPath $artifactPath -Force
} finally {
    if (Test-Path -LiteralPath $largeCircuit) {
        Remove-Item -LiteralPath $largeCircuit -Force
    }
}

Write-Host '[PASS] test50 structure_query gate-type filter regression complete'
