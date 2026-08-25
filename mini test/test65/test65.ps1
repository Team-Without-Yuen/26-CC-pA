param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\..\tools.exe')
)

$ErrorActionPreference = 'Stop'
$tools = (Resolve-Path -LiteralPath $Executable).Path
$circuit = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'constant_fanout.v')).Path
$commands = @(
    "read $circuit"
    "structure_query fanout_load 1'b1"
    'edit_apply insert_buffers_for_fanout 2'
    "structure_query fanout_load 1'b1"
    'structure_query fanout_load a'
    'exit'
) -join "`n"

$output = (($commands + "`n") | & $tools 2>&1) -join "`n"
$envelopes = [regex]::Matches($output, '(?s)TOOL_RESULT_BEGIN.*?TOOL_RESULT_END')
$editEnvelope = $envelopes | ForEach-Object { $_.Value } |
    Where-Object { $_ -match 'mode:\s*insert_buffers_for_fanout' } |
    Select-Object -First 1

$passed = 0
$failed = 0
function Assert-Check([bool]$Condition, [string]$Label) {
    if ($Condition) {
        $script:passed++
        Write-Host "[PASS] $Label"
    } else {
        $script:failed++
        Write-Host "[FAIL] $Label"
    }
}

Assert-Check ($null -ne $editEnvelope) 'edit envelope exists'
Assert-Check ($editEnvelope -match 'report_success:\s*true') 'fanout edit succeeds'
Assert-Check ($editEnvelope -match 'report_changed:\s*true') 'fanout edit changes design'
Assert-Check ($editEnvelope -match 'before_max_fanout:\s*5') 'bufferable before max is five'
Assert-Check ($editEnvelope -match 'after_max_fanout:\s*2') 'bufferable after max meets limit'
Assert-Check ($editEnvelope -match 'meets_constraint:\s*true') 'edit constraint is satisfied'
Assert-Check ($editEnvelope -notmatch "violating_net_names(?s:.*?)1'b1") 'constant is not an edit violation'
Assert-Check (([regex]::Matches($output, 'fanout load count \(QA definition\):\s*5')).Count -eq 2) 'generic constant fanout query remains five'
Assert-Check ($output -match 'net:\s*a(?s:.*?)fanout load count \(QA definition\):\s*2') 'ordinary source fanout is reduced to two'

Write-Host "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Host '--- raw output ---'
    Write-Host $output
    exit 1
}
