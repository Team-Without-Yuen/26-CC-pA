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
Require ($output.Contains('list entry count: 41')) 'artifact preserves all 40 gate records and the type-count record'
Require ($output.Contains('response token limit: 4096')) 'response uses the official 4096-token limit'
Require ($output.Contains('artifact triggered by token estimate: yes')) 'conservative token estimate triggers the artifact'
Require (-not $output.Contains('Gate connection details (40):')) 'long records are suppressed from terminal'

$match = [regex]::Match($output, '(?m)^  output_file: (.+)$')
Require $match.Success 'response returns output_file'
$artifactPath = Join-Path $root $match.Groups[1].Value.Trim()
Require (Test-Path -LiteralPath $artifactPath) 'character-triggered artifact exists'
$artifact = Get-Content -LiteralPath $artifactPath -Raw
Require ($artifact.Contains('response token limit: 4096')) 'artifact records the response token limit'
Require ($artifact.Contains('triggered by token estimate: yes')) 'artifact records the token trigger state'
Require ($artifact.Contains('Gate connection details (40):')) 'artifact contains all long records'
Require ($artifact.Contains('Complete: yes')) 'artifact footer declares completeness'
Remove-Item -LiteralPath $artifactPath -Force

Write-Host '[PASS] test54 4096-token automatic artifact regression complete'
