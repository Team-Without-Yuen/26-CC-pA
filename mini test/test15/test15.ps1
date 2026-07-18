param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$commands = @(
    "read mini test/test15/port_width_circuit.v"
    "basic_query list_pi"
    "basic_query list_po"
    "basic_query port_info data_in"
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
Check-Result ($output.Contains("Port summaries (3):")) "PI batch contains all three input ports"
Check-Result ($output.Contains("Port summaries (2):")) "PO batch contains both output ports"
Check-Result ($output.Contains("object: data_in") -and $output.Contains("width: 4")) "existing port_info remains compatible"
Check-Result (-not $output.Contains("status: error")) "all batch port queries succeed"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 5) "every command returns one envelope"
Check-Result (([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 5) "every envelope is closed"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    exit 1
}

exit 0
