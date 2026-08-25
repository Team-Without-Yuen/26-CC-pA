param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\..\tools.exe')
)

$ErrorActionPreference = 'Stop'
$tools = (Resolve-Path -LiteralPath $Executable).Path
$circuit = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'exact_xor4.v')).Path
$outputFile = Join-Path ([IO.Path]::GetTempPath()) (
    'cad_exact_xor4_' + [Guid]::NewGuid().ToString('N') + '.v')

$passed = 0
$failed = 0
function Assert-Check([bool]$Condition, [string]$Label) {
    if ($Condition) {
        $script:passed++
        Write-Host "[PASS] $Label"
    } else {
        $script:failed++
        Write-Host "[FAIL] $Label"
    }
}

function Same-Pair($Gate, [string]$Left, [string]$Right) {
    return (($Gate.A -eq $Left) -and ($Gate.B -eq $Right)) -or
           (($Gate.A -eq $Right) -and ($Gate.B -eq $Left))
}

try {
    $commands = @(
        "read $circuit"
        'edit_apply replace_type whole XOR -allow NAND'
        "write $outputFile"
        'exit'
    ) -join "`n"
    $toolOutput = (($commands + "`n") | & $tools 2>&1) -join "`n"
    $written = Get-Content -LiteralPath $outputFile -Raw

    Assert-Check ($toolOutput -match 'report_success:\s*true') 'exact XOR replacement succeeds'
    Assert-Check ($toolOutput -match 'report_changed:\s*true') 'exact XOR replacement changes design'
    Assert-Check ($toolOutput -match '(?s)removed_count_by_type \(1\):\s*XOR\s*:\s*2') 'two XOR gates are removed'
    Assert-Check ($toolOutput -match '(?s)added_count_by_type \(1\):\s*NAND\s*:\s*8') 'exactly eight NAND gates are added'
    Assert-Check ($toolOutput -match '(?s)final_gate_count_by_type \(2\):.*?NAND\s*:\s*9.*?XOR\s*:\s*0') 'final aggregate counts include the pre-existing NAND'
    Assert-Check ($written -notmatch '(?m)^\s*xor\s+') 'written design contains no XOR gates'
    Assert-Check ($written -match '(?m)^\s*nand\s+keep0\s*\(') 'unrelated NAND gate is preserved'

    $expected = @(
        [pscustomobject]@{ Name = 'x0'; Out = 'y0'; A = 'a'; B = 'b' }
        [pscustomobject]@{ Name = 'x1'; Out = 'y1'; A = 'c'; B = 'd' }
    )
    $groups = @{}
    [regex]::Matches(
        $written,
        '(?m)^\s*nand\s+((\S+)_opt_\d+)\s*\(\s*([^,]+),\s*([^,]+),\s*([^\)]+)\s*\);'
    ) | ForEach-Object {
        $base = $_.Groups[2].Value
        if (-not $groups.ContainsKey($base)) {
            $groups[$base] = [Collections.ArrayList]::new()
        }
        $null = $groups[$base].Add([pscustomobject]@{
            Name = $_.Groups[1].Value.Trim()
            Out = $_.Groups[3].Value.Trim()
            A = $_.Groups[4].Value.Trim()
            B = $_.Groups[5].Value.Trim()
        })
    }

    foreach ($item in $expected) {
        $group = @($groups[$item.Name])
        $topologyOk = $group.Count -eq 4
        if ($topologyOk) {
            $shared = $group | Where-Object {
                Same-Pair $_ $item.A $item.B
            } | Select-Object -First 1
            $branchA = $group | Where-Object {
                $null -ne $shared -and (Same-Pair $_ $item.A $shared.Out)
            } | Select-Object -First 1
            $branchB = $group | Where-Object {
                $null -ne $shared -and (Same-Pair $_ $item.B $shared.Out)
            } | Select-Object -First 1
            $root = $group | Where-Object {
                $null -ne $branchA -and $null -ne $branchB -and
                $_.Out -eq $item.Out -and
                (Same-Pair $_ $branchA.Out $branchB.Out)
            } | Select-Object -First 1
            $topologyOk = $null -ne $shared -and
                          $null -ne $branchA -and
                          $null -ne $branchB -and
                          $null -ne $root
        }
        Assert-Check $topologyOk "$($item.Name) uses the standard shared-node four-NAND topology"
    }
} finally {
    if (Test-Path -LiteralPath $outputFile) {
        Remove-Item -LiteralPath $outputFile -Force
    }
}

Write-Host "Summary: $passed passed, $failed failed."
if ($failed -ne 0) { exit 1 }
