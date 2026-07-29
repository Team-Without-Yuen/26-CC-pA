param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$outPath = "mini test/test34/floating_po_written.v"
if (Test-Path -LiteralPath $outPath) {
    Remove-Item -LiteralPath $outPath -Force
}
$commands = @(
    "read mini test/test34/floating_po_circuit.v"
    "structure_query structural_issues"
    "edit_apply trim_dead_logic"
    "structure_query list_po"
    "write $outPath"
    "read $outPath"
    "structure_query structural_issues"
    "structure_query list_po"
    "edit_apply remove_unused_nets"
    "structure_query list_po"
    "write $outPath"
    "read $outPath"
    "structure_query structural_issues"
    "structure_query list_po"
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
    ($output -match "(?s)Undriven nets.*z" -and
     $output -match "(?s)Undriven nets.*bus_out\[0\]" -and
     $output -match "(?s)Undriven nets.*bus_out\[1\]") `
    "structural issues report scalar and bus floating primary outputs"

Check-Result `
    ($output -match "operation_name: edit_apply:trim_dead_logic" -and
     $output -match "report_success: true") `
    "trim_dead_logic succeeds on a design with floating primary outputs"

Check-Result `
    ($output -match "operation_name: edit_apply:remove_unused_nets" -and
     $output -match "report_success: true") `
    "remove_unused_nets succeeds on a design with floating primary outputs"

Check-Result `
    (([regex]::Matches($output, "name: z")).Count -ge 3 -and
     ([regex]::Matches($output, "name: bus_out")).Count -ge 3) `
    "floating primary output ports remain in current and reloaded designs"

Check-Result `
    (([regex]::Matches($output, "Undriven nets \(3\):")).Count -ge 3 -and
     ([regex]::Matches($output, "(?m)^\s+z\s*$")).Count -ge 3 -and
     ([regex]::Matches($output, "(?m)^\s+bus_out\[0\]\s*$")).Count -ge 3 -and
     ([regex]::Matches($output, "(?m)^\s+bus_out\[1\]\s*$")).Count -ge 3) `
    "writer preserves floating primary outputs as undriven after reload"

Check-Result `
    (([regex]::Matches($output, "TOOL_RESULT_BEGIN")).Count -eq 15 -and
     ([regex]::Matches($output, "TOOL_RESULT_END")).Count -eq 15) `
    "every floating-PO CLI command has one response envelope"

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -ne 0) {
    Write-Output "--- tools output ---"
    Write-Output $output
    if (Test-Path -LiteralPath $outPath) {
        Remove-Item -LiteralPath $outPath -Force
    }
    exit 1
}

if (Test-Path -LiteralPath $outPath) {
    Remove-Item -LiteralPath $outPath -Force
}

exit 0
