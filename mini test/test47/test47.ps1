param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$passed = 0
$failed = 0
$createdArtifacts = @()

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

function Invoke-ToolBatch {
    param([string[]]$Commands)
    return (($Commands -join "`n") + "`n") | & $Executable 2>&1 | Out-String
}

function Get-OutputFile {
    param([string]$Output)
    $match = [regex]::Match(($Output -replace "`r", ""), '(?m)^  output_file: (.+)$')
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    return ""
}

try {
    $fanoutOutput = Invoke-ToolBatch @(
        "read NewTestCase/test65/test65.v",
        "structure_query fanout_load n0",
        "exit"
    )
    $fanoutPath = Get-OutputFile $fanoutOutput
    if ($fanoutPath) { $createdArtifacts += $fanoutPath }
    $fanoutArtifact = if ($fanoutPath -and (Test-Path -LiteralPath $fanoutPath)) {
        Get-Content -LiteralPath $fanoutPath -Raw
    } else { "" }

    Check-Result ($fanoutOutput.Contains("list artifact complete: yes")) `
        "large fanout list reports a complete artifact"
    Check-Result ($fanoutOutput.Contains("list entry count: 3802")) `
        "fanout artifact reports all 3802 categorized loads"
    Check-Result (-not $fanoutOutput.Contains("DFF clock-pin loads (3802):")) `
        "large fanout list is omitted from terminal"
    Check-Result ($fanoutArtifact.Contains("Format: QUERY_LIST_ARTIFACT_V1") -and
                  $fanoutArtifact.Contains("DFF clock-pin loads (3802):") -and
                  $fanoutArtifact.Contains("  g90") -and
                  $fanoutArtifact.Contains("Complete: yes")) `
        "fanout artifact is complete and self-contained"

    $coneOutput = Invoke-ToolBatch @(
        "read NewTestCase/test83/test83.v",
        "cone_query largest_output",
        "exit"
    )
    $conePath = Get-OutputFile $coneOutput
    if ($conePath) { $createdArtifacts += $conePath }
    $coneArtifact = if ($conePath -and (Test-Path -LiteralPath $conePath)) {
        Get-Content -LiteralPath $conePath -Raw
    } else { "" }

    Check-Result ($coneOutput.Contains("list artifact complete: yes")) `
        "large cone list reports a complete artifact"
    Check-Result (-not $coneOutput.Contains("Cone gates (2342):")) `
        "large cone gate list is omitted from terminal"
    Check-Result ($coneArtifact.Contains("Cone gates (2342):") -and
                  $coneArtifact.Contains("Complete: yes")) `
        "cone artifact contains the complete official-test gate list"

    $smallOutput = Invoke-ToolBatch @(
        "read NewTestCase/test58/test58.v",
        "structure_query list_dffs",
        "exit"
    )
    Check-Result (-not $smallOutput.Contains("list artifact format:") -and
                  $smallOutput.Contains("Gate names (8):")) `
        "small lists remain inline without an artifact"
} finally {
    foreach ($path in $createdArtifacts) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}

Write-Output "Summary: $passed passed, $failed failed."
if ($failed -gt 0) { exit 1 }
