param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$circuit = Join-Path $PSScriptRoot 'depth_large.v'
$artifactCircuit = Join-Path $PSScriptRoot 'depth_artifact_large.v'
$filterCircuit = Join-Path $PSScriptRoot 'depth_filter.v'
$statusCircuit = Join-Path $PSScriptRoot 'depth_status.v'
$criticalCircuit = Join-Path $PSScriptRoot 'critical_gates.v'
$criticalLargeCircuit = Join-Path $PSScriptRoot 'critical_gates_large.v'
$createdArtifacts = @()

function Invoke-Tools([string[]]$Commands) {
    (($Commands + 'exit') -join "`n") + "`n" | & $Executable 2>&1 | Out-String
}
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    Write-Host "[PASS] $Message"
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
$read = "read $circuit"
$output = Invoke-Tools @($read, 'depth_query all_po')
Require ($output.Contains('status: ok')) 'all_po succeeds'
Require ($output.Contains('Report count: 201')) 'all_po returns the exact endpoint count'
Require (-not $output.Contains('list artifact format:')) `
    '201 short endpoint records remain inline below the token estimate'
$inlineRecords = [regex]::Matches(
    ($output -replace "`r", ""),
    '(?m)^  shared\[[0-9]+\] status=available depth=0$')
Require ($inlineRecords.Count -eq 201) `
    'inline depth response contains all 201 endpoint records'

$artifactRead = "read $artifactCircuit"
$artifactOutput = Invoke-Tools @($artifactRead, 'depth_query all_po')
Require ($artifactOutput.Contains('status: ok')) 'large all_po succeeds'
Require ($artifactOutput.Contains('Report count: 600')) `
    'large all_po returns the exact endpoint count'
Require ($artifactOutput.Contains('list artifact complete: yes') -and
         $artifactOutput.Contains('wrote list to file: yes')) `
    'large depth result uses a complete artifact'
Require (-not $artifactOutput.Contains('  shared[0] status=')) `
    'large endpoint records are suppressed from terminal'

$match = [regex]::Match($artifactOutput, '(?m)^  output_file: (.+)$')
Require $match.Success 'response returns output_file'
$artifactPath = $match.Groups[1].Value.Trim()
if (-not [IO.Path]::IsPathRooted($artifactPath)) {
    $artifactPath = Join-Path $root $artifactPath
}
if ($artifactPath) { $createdArtifacts += $artifactPath }
Require (Test-Path -LiteralPath $artifactPath) 'depth artifact exists'
$artifact = Get-Content -LiteralPath $artifactPath -Raw
$endpointSection = Get-ArtifactSectionCount $artifact 'Endpoint depth reports'
Require ($artifact.Contains('report_count: 600')) 'artifact declares endpoint count'
Require ($null -ne $endpointSection -and
         $endpointSection.Declared -eq 600 -and
         $endpointSection.Actual -eq 600) `
    'artifact contains all 600 endpoint records'
Require ($artifact.Contains('Complete: yes')) 'artifact footer declares completeness'
Remove-Item -LiteralPath $artifactPath -Force
$createdArtifacts = @($createdArtifacts | Where-Object { $_ -ne $artifactPath })

$filterRead = "read $filterCircuit"
$filterCases = @(
    @{ Command = 'depth_query filter all eq 2'; Count = 2 },
    @{ Command = 'depth_query filter all ne 2'; Count = 2 },
    @{ Command = 'depth_query filter all gt 1'; Count = 3 },
    @{ Command = 'depth_query filter all ge 2'; Count = 3 },
    @{ Command = 'depth_query filter all lt 2'; Count = 1 },
    @{ Command = 'depth_query filter all le 2'; Count = 3 },
    @{ Command = 'depth_query filter all between 1 2'; Count = 3 }
)
foreach ($case in $filterCases) {
    $filterOutput = Invoke-Tools @($filterRead, $case.Command)
    Require ($filterOutput.Contains('status: ok') -and
             $filterOutput.Contains('Checked endpoint count: 4') -and
             $filterOutput.Contains("Matched endpoint count: $($case.Count)")) `
        "depth filter predicate result: $($case.Command)"
}

$equalOutput = Invoke-Tools @($filterRead, 'depth_query filter all eq 2')
Require ($equalOutput.Contains('  y2 type=primary_output status=available depth=2') -and
         $equalOutput.Contains('  ff0.D type=dff_d status=available depth=2')) `
    'depth filter returns typed PO and DFF.D records in stable order'

$poOutput = Invoke-Tools @($filterRead, 'depth_query filter po eq 2')
Require ($poOutput.Contains('Filter scope: po') -and
         $poOutput.Contains('Checked endpoint count: 3') -and
         $poOutput.Contains('Matched endpoint count: 1') -and
         $poOutput.Contains('  y2 type=primary_output status=available depth=2')) `
    'depth filter po scope'

$dffOutput = Invoke-Tools @($filterRead, 'depth_query filter dff_d eq 2')
Require ($dffOutput.Contains('Filter scope: dff_d') -and
         $dffOutput.Contains('Checked endpoint count: 1') -and
         $dffOutput.Contains('Matched endpoint count: 1') -and
         $dffOutput.Contains('  ff0.D type=dff_d status=available depth=2')) `
    'depth filter dff_d scope'

$zeroOutput = Invoke-Tools @($filterRead, 'depth_query filter po eq 99')
Require ($zeroOutput.Contains('status: ok') -and
         $zeroOutput.Contains('complete: true') -and
         $zeroOutput.Contains('Report count: 0') -and
         $zeroOutput.Contains('Checked endpoint count: 3') -and
         $zeroOutput.Contains('Matched endpoint count: 0') -and
         $zeroOutput.Contains('Analysis complete: yes')) `
    'depth filter zero match is a complete success'

$filterArtifactOutput = Invoke-Tools @(
    $artifactRead, 'depth_query filter po eq 0')
Require ($filterArtifactOutput.Contains('status: ok') -and
         $filterArtifactOutput.Contains('Matched endpoint count: 600') -and
         $filterArtifactOutput.Contains('list artifact complete: yes')) `
    'large depth filter reports a complete artifact'
$filterArtifactMatch = [regex]::Match(
    $filterArtifactOutput, '(?m)^  output_file: (.+)$')
Require $filterArtifactMatch.Success 'large depth filter returns output_file'
$filterArtifactPath = $filterArtifactMatch.Groups[1].Value.Trim()
if (-not [IO.Path]::IsPathRooted($filterArtifactPath)) {
    $filterArtifactPath = Join-Path $root $filterArtifactPath
}
if ($filterArtifactPath) { $createdArtifacts += $filterArtifactPath }
$filterArtifact = Get-Content -LiteralPath $filterArtifactPath -Raw
$filterSection = Get-ArtifactSectionCount $filterArtifact 'Endpoint depth reports'
Require ($filterArtifact.Contains('filter_scope: po') -and
         $filterArtifact.Contains('filter_predicate: eq') -and
         $filterArtifact.Contains('checked_endpoint_count: 600') -and
         $filterArtifact.Contains('matched_endpoint_count: 600') -and
         $null -ne $filterSection -and
         $filterSection.Declared -eq 600 -and
         $filterSection.Actual -eq 600 -and
         $filterArtifact.Contains('shared[0] type=primary_output status=available depth=0') -and
         $filterArtifact.Contains('Complete: yes')) `
    'large depth filter artifact is self-contained and complete'
Remove-Item -LiteralPath $filterArtifactPath -Force
$createdArtifacts = @($createdArtifacts | Where-Object { $_ -ne $filterArtifactPath })

$statusRead = "read $statusCircuit"
$statusOutput = Invoke-Tools @($statusRead, 'depth_query all_po')
Require ($statusOutput.Contains('status: ok') -and
         $statusOutput.Contains('complete: true') -and
         $statusOutput.Contains('Report count: 3') -and
         $statusOutput.Contains('Defined depth endpoint count: 1') -and
         $statusOutput.Contains('No timing path endpoint count: 2') -and
         $statusOutput.Contains('Graph inconsistent endpoint count: 0') -and
         $statusOutput.Contains('Analysis failure endpoint count: 0') -and
         $statusOutput.Contains('Analysis complete: yes')) `
    'legal no-timing-path outputs keep the analysis complete'
Require ($statusOutput.Contains('  defined_out status=available depth=1') -and
         $statusOutput.Contains('  undriven_out status=no_timing_path depth=-1') -and
         $statusOutput.Contains('  floating_fanout_out status=no_timing_path depth=-1')) `
    'endpoint records distinguish available, undriven, and propagated no-path'

$statusFilterOutput = Invoke-Tools @(
    $statusRead, 'depth_query filter po ge 0')
Require ($statusFilterOutput.Contains('status: ok') -and
         $statusFilterOutput.Contains('complete: true') -and
         $statusFilterOutput.Contains('Checked endpoint count: 3') -and
         $statusFilterOutput.Contains('Matched endpoint count: 1') -and
         $statusFilterOutput.Contains('Defined depth endpoint count: 1') -and
         $statusFilterOutput.Contains('No timing path endpoint count: 2') -and
         $statusFilterOutput.Contains('Analysis complete: yes')) `
    'numeric filter excludes no-path endpoints without becoming partial'

$specificNoPathOutput = Invoke-Tools @(
    $statusRead, 'depth_query net undriven_out')
Require ($specificNoPathOutput.Contains('status: ok') -and
         $specificNoPathOutput.Contains('complete: true') -and
         $specificNoPathOutput.Contains('Report count: 1') -and
         $specificNoPathOutput.Contains('No timing path endpoint count: 1') -and
         $specificNoPathOutput.Contains('  undriven_out status=no_timing_path depth=-1')) `
    'specific undriven net returns a classified complete result'

$criticalRead = "read $criticalCircuit"
$criticalOutput = Invoke-Tools @($criticalRead, 'depth_query critical_gates')
Require ($criticalOutput.Contains('status: ok') -and
         $criticalOutput.Contains('complete: true') -and
         $criticalOutput.Contains('Report count: 6') -and
         $criticalOutput.Contains('Checked combinational gate count: 7') -and
         $criticalOutput.Contains('Analyzable timing-path gate count: 6') -and
         $criticalOutput.Contains('Critical gate count: 6') -and
         $criticalOutput.Contains('No timing path gate count: 1') -and
         $criticalOutput.Contains('Graph inconsistent gate count: 0') -and
         $criticalOutput.Contains('Analysis failure gate count: 0') -and
         $criticalOutput.Contains('Critical gate type counts:') -and
         $criticalOutput.Contains('  AND : 1') -and
         $criticalOutput.Contains('  OR : 1') -and
         $criticalOutput.Contains('  NOT : 1') -and
         $criticalOutput.Contains('  BUF : 3') -and
         $criticalOutput.Contains('  NAND : 0') -and
         $criticalOutput.Contains('Worst endpoint: y0 depth=3')) `
    'critical_gates returns complete batch summary'
$criticalRecords = [regex]::Matches(
    ($criticalOutput -replace "`r", ""),
    '(?m)^  gate=(g_shared|g_left|g_left_end|g_right|g_right_end|g_dff_end) .* path_depth=3$')
Require ($criticalRecords.Count -eq 6 -and
         -not $criticalOutput.Contains('  gate=g_dangling ')) `
    'critical_gates returns the exact union and excludes dangling logic'
Require (-not $criticalOutput.Contains('  gate=ff0 ')) `
    'critical_gates excludes DFF boundary gates'

$criticalLargeRead = "read $criticalLargeCircuit"
$criticalLargeOutput = Invoke-Tools @(
    $criticalLargeRead, 'depth_query critical_gates')
Require ($criticalLargeOutput.Contains('status: ok') -and
         $criticalLargeOutput.Contains('complete: true') -and
         $criticalLargeOutput.Contains('Critical gate count: 160') -and
         $criticalLargeOutput.Contains('Critical gate type counts:') -and
         $criticalLargeOutput.Contains('  BUF : 160') -and
         $criticalLargeOutput.Contains('  NAND : 0') -and
         $criticalLargeOutput.Contains('list artifact complete: yes') -and
         $criticalLargeOutput.Contains('wrote list to file: yes')) `
    'large critical_gates result uses a complete artifact'
Require (-not $criticalLargeOutput.Contains('  gate=g0 ')) `
    'large critical gate records are suppressed from terminal'
$criticalArtifactMatch = [regex]::Match(
    $criticalLargeOutput, '(?m)^  output_file: (.+)$')
Require $criticalArtifactMatch.Success `
    'large critical_gates response returns output_file'
$criticalArtifactPath = $criticalArtifactMatch.Groups[1].Value.Trim()
if (-not [IO.Path]::IsPathRooted($criticalArtifactPath)) {
    $criticalArtifactPath = Join-Path $root $criticalArtifactPath
}
if ($criticalArtifactPath) { $createdArtifacts += $criticalArtifactPath }
$criticalArtifact = Get-Content -LiteralPath $criticalArtifactPath -Raw
$criticalSection = Get-ArtifactSectionCount `
    $criticalArtifact 'Critical gates on global maximum-depth paths'
Require ($criticalArtifact.Contains('critical_gate_count: 160') -and
         $criticalArtifact.Contains('critical_gate_type_count_BUF: 160') -and
         $criticalArtifact.Contains('critical_gate_type_count_NAND: 0') -and
         $null -ne $criticalSection -and
         $criticalSection.Declared -eq 160 -and
         $criticalSection.Actual -eq 160 -and
         $criticalArtifact.Contains('gate=g0 ') -and
         $criticalArtifact.Contains('gate=g159 ') -and
         $criticalArtifact.Contains('Complete: yes')) `
    'critical_gates artifact is self-contained and complete'
Remove-Item -LiteralPath $criticalArtifactPath -Force
$createdArtifacts = @(
    $createdArtifacts | Where-Object { $_ -ne $criticalArtifactPath })

foreach ($cmd in @(
    'depth_query all_po stray',
    'depth_query global_critical -bogus',
    'depth_query net',
    'depth_query net shared[0] stray',
    'depth_query gate_on_critical',
    'depth_query critical_gates stray',
    'depth_query exceeding',
    'depth_query exceeding -1',
    'depth_query exceeding abc',
    'depth_query exceeding 2147483648',
    'depth_query po_exceeding 1 extra',
    'depth_query filter',
    'depth_query filter invalid eq 1',
    'depth_query filter all invalid 1',
    'depth_query filter all eq',
    'depth_query filter all eq -1',
    'depth_query filter all eq 1.5',
    'depth_query filter all eq 2147483648',
    'depth_query filter all between 1',
    'depth_query filter all between 3 2',
    'depth_query filter all between 1 2 trailing')) {
    $bad = Invoke-Tools @($read, $cmd)
    Require ($bad.Contains('status: error')) "malformed input rejected: $cmd"
    $blocks = [regex]::Matches($bad, '(?s)TOOL_RESULT_BEGIN\r?\n(.*?)TOOL_RESULT_END')
    $depthBlock = @($blocks | ForEach-Object { $_.Groups[1].Value } |
        Where-Object { $_.Contains('command: depth_query') })
    Require ($depthBlock.Count -eq 1 -and -not $depthBlock[0].Contains('complete: true')) `
        "rejected input never reports complete: $cmd"
}

Require ((Invoke-Tools @($read, 'depth_query net shared[0]')).Contains('status: ok')) `
    'valid specific-net query remains supported'
Require ((Invoke-Tools @($read, 'depth_query po_exceeding 0')).Contains('status: ok')) `
    'valid threshold query remains supported'

Write-Host '[PASS] test53 depth parser and automatic artifact regression complete'
} finally {
    foreach ($path in $createdArtifacts) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}
