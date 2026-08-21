param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..\\..')
$circuit = Join-Path $PSScriptRoot 'long_record_artifact.v'
$commands = "read $circuit`nstructure_query gates_by_type AND --with-pins`nexit`n"
$output = $commands | & $Executable 2>&1 | Out-String

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    Write-Host "[PASS] $Message"
}

Require ($output.Contains('status: ok')) 'long-record query succeeds'
Require ($output.Contains('  gates: 40')) 'all 40 records are represented'
Require ($output.Contains('list entry count: 41')) '40 gate records plus one type-count record stay below the 200-entry threshold'
Require ($output.Contains('artifact triggered by entry count: no')) 'entry threshold did not trigger'
Require ($output.Contains('artifact triggered by character count: yes')) 'character threshold triggered the artifact'
Require (-not $output.Contains('Gate connection details (40):')) 'long records are suppressed from terminal'

$match = [regex]::Match($output, '(?m)^  output_file: (.+)$')
Require $match.Success 'response returns output_file'
$artifactPath = Join-Path $root $match.Groups[1].Value.Trim()
Require (Test-Path -LiteralPath $artifactPath) 'character-triggered artifact exists'
$artifact = Get-Content -LiteralPath $artifactPath -Raw
Require ($artifact.Contains('triggered by entry count: no')) 'artifact records the entry trigger state'
Require ($artifact.Contains('triggered by character count: yes')) 'artifact records the character trigger state'
Require ($artifact.Contains('Gate connection details (40):')) 'artifact contains all long records'
Require ($artifact.Contains('Complete: yes')) 'artifact footer declares completeness'
Remove-Item -LiteralPath $artifactPath -Force

Write-Host '[PASS] test54 hybrid automatic artifact threshold regression complete'
