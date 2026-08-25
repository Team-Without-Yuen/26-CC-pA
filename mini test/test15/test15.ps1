param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test15/port_width_circuit.v"
    "basic_query list_pi"
    "basic_query list_po"
    "basic_query port_info data_in"
    "basic_query port_info ascending_in"
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

Check-Result ($output.Contains("mode: list_pi")) "list_pi is routed"
Check-Result ($output.Contains("mode: list_po")) "list_po is routed"
Check-Result `
    ([regex]::IsMatch($output, "(?s)- name: data_in\r?\n    direction: input\r?\n    width: 4\r?\n    is_bus: true\r?\n    msb: 3\r?\n    lsb: 0")) `
    "input bus includes width and declared range"
Check-Result `
    ([regex]::IsMatch($output, "(?s)- name: clk\r?\n    direction: input\r?\n    width: 1\r?\n    is_bus: false\r?\n    msb: -1\r?\n    lsb: -1")) `
    "scalar input is reported as width one"
Check-Result `
    ([regex]::IsMatch($output, "(?s)- name: result\r?\n    direction: output\r?\n    width: 8\r?\n    is_bus: true\r?\n    msb: 7\r?\n    lsb: 0")) `
    "output bus includes width and declared range"
Check-Result `
    ([regex]::IsMatch($output, "(?s)- name: done\r?\n    direction: output\r?\n    width: 1\r?\n    is_bus: false")) `
    "scalar output is reported as width one"
Check-Result ($output.Contains("Port summaries (4):")) "PI batch contains all four input ports"
Check-Result ($output.Contains("Port summaries (3):")) "PO batch contains all three output ports"
Check-Result ($output.Contains("primary inputs: 4")) "PI batch preserves port count"
Check-Result ($output.Contains("primary input bits: 9")) "PI batch reports aggregate bit count"
Check-Result ($output.Contains("primary outputs: 3")) "PO batch preserves port count"
Check-Result ($output.Contains("primary output bits: 11")) "PO batch reports aggregate bit count"
Check-Result `
    ([regex]::IsMatch($output, "(?s)object: data_in.*?Port summaries \(1\):.*?- name: data_in.*?width: 4.*?msb: 3.*?lsb: 0")) `
    "descending port_info includes one declaration-range summary"
Check-Result `
    ([regex]::IsMatch($output, "(?s)object: ascending_in.*?Net names \(3\):\r?\n  ascending_in\[0\]\r?\n  ascending_in\[1\]\r?\n  ascending_in\[2\].*?Port summaries \(1\):.*?- name: ascending_in.*?width: 3.*?msb: 0.*?lsb: 2")) `
    "ascending port_info preserves declaration bounds and bit order"
Check-Result (-not $output.Contains("status: error")) "all batch port queries succeed"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 6) "every command returns one envelope"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 6) "every envelope is closed"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
