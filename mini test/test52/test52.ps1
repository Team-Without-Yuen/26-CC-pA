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
Require (-not $output.Contains('list artifact complete:')) 'sub-4096-token result stays in the response'
Require ($output.Contains('Connection 1:')) 'first direct connection is present'
Require ($output.Contains('Connection 201:')) 'last direct connection is present'
$after = @(Get-ChildItem -LiteralPath $root -Filter 'direct_pi_po_large_path_query_list_*.txt' `
    -File -ErrorAction SilentlyContinue | ForEach-Object FullName)
Require (@($after | Where-Object { $_ -notin $before }).Count -eq 0) 'test leaves no artifact behind'

Write-Host '[PASS] test52 direct_pi_po 4096-token inline regression complete'
