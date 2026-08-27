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

function Get-ArtifactSectionCount {
    param([string]$Text, [string]$Title)
    $lines = ($Text -replace "`r", "") -split "`n"
    $escapedTitle = [regex]::Escape($Title)
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $match = [regex]::Match($lines[$index], "^$escapedTitle \(([0-9]+)\):$")
        if (-not $match.Success) { continue }

        $actual = 0
        for ($entry = $index + 1; $entry -lt $lines.Count; $entry++) {
            if (-not $lines[$entry].StartsWith('  ')) { break }
            $actual++
        }
        return [PSCustomObject]@{
            Declared = [int]$match.Groups[1].Value
            Actual = $actual
        }
    }
    return $null
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
    Check-Result ($fanoutOutput.Contains("fanout load count (QA definition): 3802")) `
        "fanout envelope reports all 3802 pin-level loads"
    $directLoadSection = Get-ArtifactSectionCount $fanoutArtifact "Direct load gates"
    Check-Result ($null -ne $directLoadSection -and
                  $directLoadSection.Declared -eq 3802 -and
                  $directLoadSection.Actual -eq 3802) `
        "fanout artifact reports all 3802 distinct direct-load gates"
    $clockLoadSection = Get-ArtifactSectionCount $fanoutArtifact "DFF clock-pin loads"
    Check-Result ($null -ne $clockLoadSection -and
                  $clockLoadSection.Declared -eq 3802 -and
                  $clockLoadSection.Actual -eq 3802) `
        "fanout artifact reports all 3802 categorized clock-pin loads"
    Check-Result ($fanoutOutput.Contains("DFF clock-pin load count: 3802")) `
        "large fanout envelope retains the DFF clock-load count"
    Check-Result ($fanoutOutput.Contains("combinational gate input load count: 0") -and
                  $fanoutOutput.Contains("DFF D-pin load count: 0") -and
                  $fanoutOutput.Contains("DFF reset/set-pin load count: 0") -and
                  $fanoutOutput.Contains("DFF other-pin load count: 0")) `
        "large fanout envelope retains explicit zero category counts"
    Check-Result (-not $fanoutOutput.Contains("DFF clock-pin loads (3802):")) `
        "large fanout list is omitted from terminal"
    Check-Result ($fanoutArtifact.Contains("Format: QUERY_LIST_ARTIFACT_V1") -and
                  $fanoutArtifact.Contains("DFF clock-pin load count: 3802") -and
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

    $boundaryConeOutput = Invoke-ToolBatch @(
        "read NewTestCase/test16/test16.v",
        "cone_query net_fanin n33[0]",
        "exit"
    )
    $boundaryConePath = Get-OutputFile $boundaryConeOutput
    if ($boundaryConePath) { $createdArtifacts += $boundaryConePath }
    $boundaryConeArtifact = if ($boundaryConePath -and
        (Test-Path -LiteralPath $boundaryConePath)) {
        Get-Content -LiteralPath $boundaryConePath -Raw
    } else { "" }
    $boundaryGateSection = Get-ArtifactSectionCount `
        $boundaryConeArtifact "Cone gates"
    $boundaryNetSection = Get-ArtifactSectionCount `
        $boundaryConeArtifact "Cone nets"

    Check-Result ($boundaryConeOutput.Contains("scope gates: 388") -and
                  $boundaryConeOutput.Contains("nets: 411")) `
        "boundary cone preserves official gate and net counts"
    Check-Result ($boundaryConeOutput.Contains("list artifact complete: yes") -and
                  $boundaryConeOutput.Contains("wrote list to file: yes")) `
        "boundary cone triggers a complete artifact"
    Check-Result (-not $boundaryConeOutput.Contains("Cone gates (388):") -and
                  -not $boundaryConeOutput.Contains("Cone nets (411):")) `
        "boundary cone lists are omitted from terminal"
    Check-Result ($null -ne $boundaryGateSection -and
                  $boundaryGateSection.Declared -eq 388 -and
                  $boundaryGateSection.Actual -eq 388) `
        "boundary cone artifact contains all 388 gates"
    Check-Result ($null -ne $boundaryNetSection -and
                  $boundaryNetSection.Declared -eq 411 -and
                  $boundaryNetSection.Actual -eq 411 -and
                  $boundaryConeArtifact.Contains("Complete: yes")) `
        "boundary cone artifact contains all 411 nets and is complete"

    $fanoutFilterOutput = Invoke-ToolBatch @(
        "read NewTestCase/test84/test84.v",
        "structure_query fanout_filter all ge 0",
        "exit"
    )
    $fanoutFilterPath = Get-OutputFile $fanoutFilterOutput
    if ($fanoutFilterPath) { $createdArtifacts += $fanoutFilterPath }
    $fanoutFilterArtifact = if ($fanoutFilterPath -and
        (Test-Path -LiteralPath $fanoutFilterPath)) {
        Get-Content -LiteralPath $fanoutFilterPath -Raw
    } else { "" }
    $matchedCountMatch = [regex]::Match(
        ($fanoutFilterOutput -replace "`r", ""),
        '(?m)^  matched net count: ([0-9]+)$')
    $matchedCount = if ($matchedCountMatch.Success) {
        $matchedCountMatch.Groups[1].Value
    } else { "" }

    Check-Result ($fanoutFilterOutput.Contains("list artifact complete: yes") -and
                  $fanoutFilterOutput.Contains("wrote list to file: yes")) `
        "large fanout filter reports a complete artifact"
    Check-Result ($matchedCount -ne "" -and
                  -not $fanoutFilterOutput.Contains("Matched nets ($matchedCount):")) `
        "large fanout filter keeps the complete matched list out of the terminal"
    Check-Result ($matchedCount -ne "" -and
                  $fanoutFilterArtifact.Contains("fanout filter scope: all") -and
                  $fanoutFilterArtifact.Contains("fanout predicate: ge") -and
                  $fanoutFilterArtifact.Contains("fanout value: 0") -and
                  $fanoutFilterArtifact.Contains("matched net count: $matchedCount") -and
                  $fanoutFilterArtifact.Contains("Matched nets ($matchedCount):") -and
                  $fanoutFilterArtifact.Contains("Complete: yes")) `
        "fanout filter artifact is complete and self-contained"

    $fanoutRankOutput = Invoke-ToolBatch @(
        "read NewTestCase/test84/test84.v",
        "structure_query fanout_rank all top 999999",
        "exit"
    )
    $fanoutRankPath = Get-OutputFile $fanoutRankOutput
    if ($fanoutRankPath) { $createdArtifacts += $fanoutRankPath }
    $fanoutRankArtifact = if ($fanoutRankPath -and
        (Test-Path -LiteralPath $fanoutRankPath)) {
        Get-Content -LiteralPath $fanoutRankPath -Raw
    } else { "" }
    $rankedCountMatch = [regex]::Match(
        ($fanoutRankOutput -replace "`r", ""),
        '(?m)^  result net count: ([0-9]+)$')
    $rankedCount = if ($rankedCountMatch.Success) {
        $rankedCountMatch.Groups[1].Value
    } else { "" }

    Check-Result ($fanoutRankOutput.Contains("list artifact complete: yes") -and
                  $fanoutRankOutput.Contains("wrote list to file: yes")) `
        "large fanout ranking reports a complete artifact"
    Check-Result ($rankedCount -ne "" -and
                  -not $fanoutRankOutput.Contains("Ranked nets ($rankedCount):")) `
        "large fanout ranking keeps the complete ranked list out of the terminal"
    Check-Result ($rankedCount -ne "" -and
                  $fanoutRankArtifact.Contains("fanout ranking scope: all") -and
                  $fanoutRankArtifact.Contains("fanout ranking mode: top") -and
                  $fanoutRankArtifact.Contains("result net count: $rankedCount") -and
                  $fanoutRankArtifact.Contains("Ranked nets ($rankedCount):") -and
                  $fanoutRankArtifact.Contains("rank=1 net=") -and
                  $fanoutRankArtifact.Contains("Complete: yes")) `
        "fanout ranking artifact is complete and self-contained"

    $portOutput = Invoke-ToolBatch @(
        "read mini test/test53/depth_artifact_large.v",
        "structure_query port_info shared",
        "exit"
    )
    $portPath = Get-OutputFile $portOutput
    if ($portPath) { $createdArtifacts += $portPath }
    $portArtifact = if ($portPath -and (Test-Path -LiteralPath $portPath)) {
        Get-Content -LiteralPath $portPath -Raw
    } else { "" }

    Check-Result ($portOutput.Contains("list artifact complete: yes") -and
                  $portOutput.Contains("  width: 600") -and
                  $portOutput.Contains("  declaration_left_bound: 599") -and
                  $portOutput.Contains("  declaration_right_bound: 0")) `
        "large port envelope retains declaration bounds"
    Check-Result (-not $portOutput.Contains("Net names (600):") -and
                  -not $portOutput.Contains("Port summaries (1):")) `
        "large port lists remain outside the terminal"
    Check-Result ($portArtifact.Contains("port width: 600") -and
                  $portArtifact.Contains("port declaration left bound: 599") -and
                  $portArtifact.Contains("port declaration right bound: 0") -and
                  $portArtifact.Contains("Net names (600):") -and
                  $portArtifact.Contains("Complete: yes")) `
        "large port artifact is complete and self-contained"

    $mediumConeOutput = Invoke-ToolBatch @(
        "read NewTestCase/test53/test53.v",
        "cone_query net_fanout n2",
        "exit"
    )
    $mediumConePath = Get-OutputFile $mediumConeOutput
    if ($mediumConePath) { $createdArtifacts += $mediumConePath }
    $mediumConeArtifact = if ($mediumConePath -and
        (Test-Path -LiteralPath $mediumConePath)) {
        Get-Content -LiteralPath $mediumConePath -Raw
    } else { "" }
    $mediumGateSection = Get-ArtifactSectionCount `
        $mediumConeArtifact "Cone gates"
    $mediumNetSection = Get-ArtifactSectionCount `
        $mediumConeArtifact "Cone nets"

    Check-Result ($mediumConeOutput.Contains("list artifact complete: yes") -and
                  $mediumConeOutput.Contains("artifact entry trigger: 256") -and
                  $mediumConeOutput.Contains("artifact triggered by entry count: yes")) `
        "medium cone uses the early record-count artifact trigger"
    Check-Result (-not $mediumConeOutput.Contains("Cone gates (343):") -and
                  -not $mediumConeOutput.Contains("Cone nets (344):")) `
        "medium cone lists are omitted from the terminal"
    Check-Result ($null -ne $mediumGateSection -and
                  $mediumGateSection.Declared -eq 343 -and
                  $mediumGateSection.Actual -eq 343 -and
                  $null -ne $mediumNetSection -and
                  $mediumNetSection.Declared -eq 344 -and
                  $mediumNetSection.Actual -eq 344 -and
                  $mediumConeArtifact.Contains("artifact entry trigger: 256") -and
                  $mediumConeArtifact.Contains("Complete: yes")) `
        "medium cone artifact is complete and records its trigger contract"

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
