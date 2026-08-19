$ErrorActionPreference = 'Stop'

# path_query option strictness regression (PATH-001)
#
# 修正前：path_query 會靜默丟棄未識別的 token，仍回 status: ok 與 complete: true，
#         等於用「另一個問題」的答案回報完整結果，呼叫端無從察覺。
# 修正後：未識別 token 一律回明確 error，行為與 cone_query / structure_query 一致。
#
# fixture path_options.v 基準（已實測）：
#   6 gates；a->y 共 2 條路徑；m 為必經點；avoid m = 0、avoid p = 1
#   a->DFF.D 共 2 條；max_depth(a,y) = 3

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tools = Join-Path $root 'tools.exe'
$circuit = Join-Path $PSScriptRoot 'path_options.v'
$artifactsBefore = @(Get-ChildItem -LiteralPath $root -Filter 'path_options_path_query_*.txt' `
    -File -ErrorAction SilentlyContinue | ForEach-Object FullName)

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
# 1. 七個已記載的 option 必須全部維持可用（相容性保護）
# ---------------------------------------------------------------------------

$base = Invoke-Tools @($read, 'path_query enumerate pi:a po:y -count_only')
Assert-Contains $base 'status: ok' 'baseline enumerate succeeds'
Assert-Contains $base 'Total paths: 2' 'baseline path count'

Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a po:y -req net:m -count_only')) `
    'Total paths: 2' '-req still works'
Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a po:y -avoid net:m -count_only')) `
    'Total paths: 0' '-avoid still works'
Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a po:y -avoid net:p -count_only')) `
    'Total paths: 1' '-avoid partial still works'
Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a po:y -max_print 1')) `
    'status: ok' '-max_print still works'
Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a po:y -max_paths 10 -count_only')) `
    'Total paths: 2' '-max_paths still works'
Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a po:y -time_limit 5 -count_only')) `
    'Total paths: 2' '-time_limit still works'

$outFile = 'test51_paths_out.txt'
$resolvedOutFile = Join-Path (Get-Location) $outFile
if (Test-Path $resolvedOutFile) { Remove-Item $resolvedOutFile }
Assert-Contains (Invoke-Tools @($read, "path_query enumerate pi:a po:y -out $outFile")) `
    'status: ok' '-out still works'
if (-not (Test-Path $resolvedOutFile)) { throw '[-out] expected output file was not created' }
Write-Host '[PASS] -out wrote the artifact'
Remove-Item $resolvedOutFile

# 多 option 混用與順序
Assert-Contains (Invoke-Tools @($read,
    'path_query enumerate pi:a po:y -req net:m -count_only -time_limit 5 -max_paths 10')) `
    'Total paths: 2' 'mixed options in one command'

# 其他 mode 未受影響
Assert-Contains (Invoke-Tools @($read, 'path_query exists pi:a po:y')) 'status: ok' 'exists unaffected'
Assert-Contains (Invoke-Tools @($read, 'path_query max_depth pi:a po:y')) 'Depth: 3' 'max_depth unaffected'
Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a dff_d:g_ff -count_only')) `
    'Total paths: 2' 'DFF.D endpoint unaffected'
Assert-Contains (Invoke-Tools @($read, 'path_query direct_pi_po')) 'status: ok' 'direct_pi_po unaffected'

# ---------------------------------------------------------------------------
# 2. 未識別 option 必須回 error，不得靜默成功
#    （修正前這五個全部回 status: ok 與 Total paths: 2）
# ---------------------------------------------------------------------------

$bogus = @(
    'path_query enumerate pi:a po:y -count_only -length 5',
    'path_query enumerate pi:a po:y -count_only -bogus',
    'path_query enumerate pi:a po:y -count_only --nonsense 99',
    'path_query enumerate pi:a po:y -count_only -max_depth 2',
    'path_query enumerate pi:a po:y -count_only -exclude_type NAND'
)
foreach ($cmd in $bogus) {
    $out = Invoke-Tools @($read, $cmd)
    Assert-Contains $out 'status: error' "rejected: $cmd"
    Assert-Contains $out 'Unknown path_query option' "diagnostic names the option: $cmd"
    Assert-NotContains $out 'Total paths:' "no result is reported for: $cmd"
}

# 錯誤訊息要列出支援的 option，讓呼叫端能自行修正
Assert-Contains (Invoke-Tools @($read, 'path_query enumerate pi:a po:y -bogus')) `
    '-req -avoid -count_only' 'error lists public options only'

# 未識別 option 出現在 -req / -avoid 清單中間時，也要當成 option 而非節點名稱
foreach ($cmd in @(
    'path_query enumerate pi:a po:y -req net:m -length 5',
    'path_query enumerate pi:a po:y -avoid net:p -bogus 7')) {
    $out = Invoke-Tools @($read, $cmd)
    Assert-Contains $out 'Unknown path_query option' "dash token inside a node list is an option: $cmd"
}

# ---------------------------------------------------------------------------
# 3. 迷路的裸 token 也要回 error
# ---------------------------------------------------------------------------

$stray = Invoke-Tools @($read, 'path_query enumerate pi:a po:y net:m')
Assert-Contains $stray 'status: error' 'stray bare token rejected'
Assert-Contains $stray "Unexpected token 'net:m'" 'stray token diagnostic names the token'
Assert-NotContains $stray 'Total paths:' 'no result reported for stray token'

# ---------------------------------------------------------------------------
# 4. 需要參數的 option 缺值時要回 error，不得靜默忽略
# ---------------------------------------------------------------------------

foreach ($pair in @(
    @('path_query enumerate pi:a po:y -out',        '-out requires'),
    @('path_query enumerate pi:a po:y -out -count_only', '-out requires'),
    @('path_query enumerate pi:a po:y -max_print',  '-max_print requires'),
    @('path_query enumerate pi:a po:y -max_paths',  '-max_paths requires'),
    @('path_query enumerate pi:a po:y -time_limit', '-time_limit requires'))) {
    $out = Invoke-Tools @($read, $pair[0])
    Assert-Contains $out 'status: error' "missing value rejected: $($pair[0])"
    Assert-Contains $out $pair[1] "missing value diagnostic: $($pair[0])"
}

# 數值必須完整解析，不能把負數 wrap 成 size_t，也不能接受 overflow / NaN / Inf。
foreach ($pair in @(
    @('path_query enumerate pi:a po:y -max_print -1', '-max_print requires'),
    @('path_query enumerate pi:a po:y -max_paths -1', '-max_paths requires'),
    @('path_query enumerate pi:a po:y -max_paths 184467440737095516160', '-max_paths requires'),
    @('path_query enumerate pi:a po:y -time_limit 0', '-time_limit requires'),
    @('path_query enumerate pi:a po:y -time_limit -1', '-time_limit requires'),
    @('path_query enumerate pi:a po:y -time_limit nan', '-time_limit requires'),
    @('path_query enumerate pi:a po:y -time_limit inf', '-time_limit requires'))) {
    $out = Invoke-Tools @($read, $pair[0])
    Assert-Contains $out 'status: error' "invalid numeric value rejected: $($pair[0])"
    Assert-Contains $out $pair[1] "invalid numeric diagnostic: $($pair[0])"
}

# list option 不得為空；下一個 option 不能被當成 node。
foreach ($cmd in @(
    'path_query enumerate pi:a po:y -req',
    'path_query enumerate pi:a po:y -avoid -count_only')) {
    $out = Invoke-Tools @($read, $cmd)
    Assert-Contains $out 'status: error' "empty node list rejected: $cmd"
    Assert-Contains $out 'requires at least one node' "empty node list diagnostic: $cmd"
}

# Legacy register wrapper 使用相同的 fail-closed 規則。
foreach ($cmd in @(
    'reg_path_query enumerate -bogus',
    'reg_path_query enumerate stray',
    'reg_path_query enumerate -out -count_only',
    'reg_path_query enumerate -max_paths -1',
    'reg_path_query enumerate -time_limit nan',
    'reg_path_query enumerate -from -count_only')) {
    $out = Invoke-Tools @($read, $cmd)
    Assert-Contains $out 'status: error' "reg_path_query rejected malformed input: $cmd"
}

# ---------------------------------------------------------------------------
# 5. 核心迴歸鎖：被拒絕的輸入不得宣稱結果完整
#    只檢查 path_query 自己的 envelope；read / exit 的 block 本來就會回 complete: true
# ---------------------------------------------------------------------------

foreach ($cmd in ($bogus + @('path_query enumerate pi:a po:y net:m'))) {
    $out = Invoke-Tools @($read, $cmd)
    $blocks = [regex]::Matches($out, '(?s)TOOL_RESULT_BEGIN\r?\n(.*?)TOOL_RESULT_END')
    $pq = @($blocks | ForEach-Object { $_.Groups[1].Value } |
            Where-Object { $_.Contains('command: path_query') })
    if ($pq.Count -ne 1) {
        throw "[complete-lock] expected exactly one path_query envelope for: $cmd"
    }
    if ($pq[0].Contains('complete: true')) {
        throw "[complete-lock] rejected input reported complete: true for: $cmd`n$($pq[0])"
    }
    if (-not $pq[0].Contains('status: error')) {
        throw "[complete-lock] rejected input did not report status: error for: $cmd`n$($pq[0])"
    }
    Write-Host "[PASS] rejected input never reports complete: true -- $cmd"
}

$artifactsAfter = @(Get-ChildItem -LiteralPath $root -Filter 'path_options_path_query_*.txt' `
    -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
foreach ($artifact in @($artifactsAfter | Where-Object { $_ -notin $artifactsBefore })) {
    Remove-Item -LiteralPath $artifact -Force
}

Write-Host '[PASS] test51 path_query option strictness regression complete'
