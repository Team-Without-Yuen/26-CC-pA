param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test16/active_design_circuit.v"
    "basic_query summary"
    "edit_apply simplify_constants AND 0"
    "edit_apply remove_unused_nets"
    "basic_query summary"
    "basic_query list_gates"
    "basic_query list_nets"
    "basic_query count_by_type"
    "basic_query gate_info g_dead"
    "basic_query net_info dead_logic"
    "equiv_query original 5"
    "quit"
) -join "`n"

$output = $commands | & $Executable 2>&1 | Out-String
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

Check-Result `
    ($output -match "(?s)OK: Basic design summary.*?gates: 2.*?nets: 4.*?logical wires: 4") `
    "initial summary reports all active objects"
Check-Result `
    ($output -match "(?s)OK: Basic design summary.*?gates: 1.*?nets: 3.*?logical wires: 3") `
    "edited summary excludes gate and net tombstones"
Check-Result `
    ($output -match "Gate names \(1\):\s+g_live") `
    "gate list excludes removed g_dead tombstone"
Check-Result `
    ($output -match "Net names \(3\):\s+a\s+y\s+1'b0") `
    "net list excludes removed dead_logic tombstone"
Check-Result `
    ($output -match "(?s)OK: Count gates by type.*?gates: 1.*?BUF : 1" -and `
     -not $output.Contains("UNKNOWN : 1")) `
    "unfiltered type count reports the active gate total"
Check-Result `
    ($output.Contains("Error: Gate not found: g_dead")) `
    "gate_info rejects a removed gate"
Check-Result `
    ($output.Contains("Error: Net not found: dead_logic")) `
    "net_info rejects a removed net"
Check-Result `
    ($output.Contains("equivalent: true")) `
    "active-object cleanup preserves whole-design function"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
