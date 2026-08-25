param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$circuit = Join-Path $PSScriptRoot 'depth_large.v'
$artifactCircuit = Join-Path $PSScriptRoot 'depth_artifact_large.v'
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
    ($output -replace "`r", ""), '(?m)^  shared\[[0-9]+\] depth=0$')
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
Require (-not $artifactOutput.Contains('  shared[0] depth=')) `
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

foreach ($cmd in @(
    'depth_query all_po stray',
    'depth_query global_critical -bogus',
    'depth_query net',
    'depth_query net shared[0] stray',
    'depth_query gate_on_critical',
    'depth_query exceeding',
    'depth_query exceeding -1',
    'depth_query exceeding abc',
    'depth_query exceeding 2147483648',
    'depth_query po_exceeding 1 extra')) {
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
