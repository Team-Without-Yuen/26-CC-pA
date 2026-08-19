param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$circuit = Join-Path $PSScriptRoot 'direct_pi_po_large.v'
$before = @(Get-ChildItem -LiteralPath $root -Filter 'direct_pi_po_large_path_query_list_*.txt' `
    -File -ErrorAction SilentlyContinue | ForEach-Object FullName)

$commands = "read $circuit`npath_query direct_pi_po`nexit`n"
$output = $commands | & $Executable 2>&1 | Out-String

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    Write-Host "[PASS] $Message"
}

Require ($output.Contains('status: ok')) 'direct_pi_po succeeds'
Require ($output.Contains('Total direct PI-to-PO connections: 201')) 'exact connection count is returned'
Require ($output.Contains('list artifact complete: yes')) 'large result uses a complete artifact'
Require ($output.Contains('list entry count: 201')) 'artifact metadata has the complete count'
Require ($output.Contains('artifact triggered by entry count: yes')) 'large short-record set triggers by entry count'
Require ($output.Contains('artifact triggered by character count: no')) 'short records stay below character threshold'
Require (-not $output.Contains('Connection 1:')) 'large records are suppressed from terminal output'

$match = [regex]::Match($output, '(?m)^  output_file: (.+)$')
Require $match.Success 'response returns output_file'
$artifactPath = Join-Path $root $match.Groups[1].Value.Trim()
Require (Test-Path -LiteralPath $artifactPath) 'artifact file exists'
$artifact = Get-Content -LiteralPath $artifactPath -Raw
Require ($artifact.Contains('Format: QUERY_LIST_ARTIFACT_V1')) 'artifact declares its format'
Require ($artifact.Contains('total_direct_connections: 201')) 'artifact is self-describing'
Require ($artifact.Contains('Direct PI-to-PO connections (201):')) 'artifact contains every record'
Require ($artifact.Contains('Complete: yes')) 'artifact footer declares completeness'

Remove-Item -LiteralPath $artifactPath -Force
$after = @(Get-ChildItem -LiteralPath $root -Filter 'direct_pi_po_large_path_query_list_*.txt' `
    -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
Require (@($after | Where-Object { $_ -notin $before }).Count -eq 0) 'test leaves no artifact behind'

Write-Host '[PASS] test52 direct_pi_po automatic artifact regression complete'
