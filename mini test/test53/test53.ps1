param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$circuit = Join-Path $PSScriptRoot 'depth_large.v'

function Invoke-Tools([string[]]$Commands) {
    (($Commands + 'exit') -join "`n") + "`n" | & $Executable 2>&1 | Out-String
}
function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    Write-Host "[PASS] $Message"
}

$read = "read $circuit"
$output = Invoke-Tools @($read, 'depth_query all_po')
Require ($output.Contains('status: ok')) 'all_po succeeds'
Require ($output.Contains('Report count: 201')) 'all_po returns the exact endpoint count'
Require ($output.Contains('list artifact complete: yes')) 'large depth result uses a complete artifact'
Require (-not $output.Contains('  shared[0] depth=')) 'endpoint records are suppressed from terminal'

$match = [regex]::Match($output, '(?m)^  output_file: (.+)$')
Require $match.Success 'response returns output_file'
$artifactPath = Join-Path $root $match.Groups[1].Value.Trim()
Require (Test-Path -LiteralPath $artifactPath) 'depth artifact exists'
$artifact = Get-Content -LiteralPath $artifactPath -Raw
Require ($artifact.Contains('report_count: 201')) 'artifact declares endpoint count'
Require ($artifact.Contains('Endpoint depth reports (201):')) 'artifact contains all endpoint records'
Require ($artifact.Contains('Complete: yes')) 'artifact footer declares completeness'
Remove-Item -LiteralPath $artifactPath -Force

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
