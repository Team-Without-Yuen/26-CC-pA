$ErrorActionPreference = 'Stop'

# FunctionReport automatic-list-artifact regression (FUNCTION-OUTPUT-001/002).

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tools = Join-Path $root 'tools.exe'
$circuit = Join-Path $PSScriptRoot 'function_large_support.v'
$createdArtifacts = @()

function Invoke-Tools([string[]] $Commands) {
    $inputText = (($Commands + 'exit') -join "`n") + "`n"
    return ($inputText | & $tools 2>&1) -join "`n"
}

function Assert-Contains([string] $Text, [string] $Expected, [string] $Label) {
    if (-not $Text.Contains($Expected)) {
        throw "[$Label] Missing expected text: $Expected`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

function Assert-NotContains([string] $Text, [string] $Unexpected, [string] $Label) {
    if ($Text.Contains($Unexpected)) {
        throw "[$Label] Unexpected text present: $Unexpected`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

function Get-OutputFiles([string] $Text) {
    return [regex]::Matches(($Text -replace "`r", ''), '(?m)^  output_file: (.+)$') |
        ForEach-Object { $_.Groups[1].Value.Trim() }
}

try {
    $verilog = [System.Text.StringBuilder]::new()
    [void]$verilog.AppendLine('module function_large_support (')
    [void]$verilog.AppendLine('    input [219:0] p,')
    [void]$verilog.AppendLine('    output y')
    [void]$verilog.AppendLine(');')
    for ($i = 3; $i -le 219; $i++) {
        [void]$verilog.AppendLine("    wire n$i;")
    }
    [void]$verilog.AppendLine('    wire a_term;')
    [void]$verilog.AppendLine('    or g_or_3 (n3, p[2], p[3]);')
    for ($i = 4; $i -le 219; $i++) {
        $previous = $i - 1
        [void]$verilog.AppendLine("    or g_or_$i (n$i, n$previous, p[$i]);")
    }
    [void]$verilog.AppendLine('    and g_and (a_term, p[0], n219);')
    [void]$verilog.AppendLine('    or g_out (y, a_term, p[1]);')
    [void]$verilog.AppendLine('endmodule')
    Set-Content -LiteralPath $circuit -Value $verilog.ToString() -Encoding ascii

    $read = "read $circuit"

    $support = Invoke-Tools @($read, 'func_query support_pi y')
    $supportFiles = @(Get-OutputFiles $support)
    $createdArtifacts += $supportFiles
    Assert-Contains $support 'status: ok' 'support query succeeds'
    Assert-Contains $support 'complete: true' 'support query is complete'
    Assert-Contains $support 'support leaf count: 220' 'support count remains inline'
    Assert-Contains $support 'list artifact complete: yes' 'support list artifact is complete'
    Assert-Contains $support 'wrote list to file: yes' 'support list is written'
    Assert-NotContains $support 'Support leaves (220):' 'large support list is omitted from terminal'
    if ($supportFiles.Count -ne 1 -or -not (Test-Path -LiteralPath $supportFiles[0])) {
        throw '[support artifact] Expected exactly one existing output file.'
    }
    $supportArtifact = Get-Content -LiteralPath $supportFiles[0] -Raw
    Assert-Contains $supportArtifact 'Format: QUERY_LIST_ARTIFACT_V1' 'support artifact format'
    Assert-Contains $supportArtifact 'Support leaves (220):' 'support artifact has complete union'
    Assert-Contains $supportArtifact 'Real primary inputs (220):' 'support artifact has PI classification'
    Assert-Contains $supportArtifact 'Complete: yes' 'support artifact footer is complete'

    $symmetry = Invoke-Tools @($read, 'func_query symmetry y p[0] p[1]')
    $symmetryFiles = @(Get-OutputFiles $symmetry)
    $createdArtifacts += $symmetryFiles
    Assert-Contains $symmetry 'status: ok' 'symmetry query succeeds'
    Assert-Contains $symmetry 'complete: true' 'symmetry query is complete'
    Assert-Contains $symmetry 'symmetric: no' 'fixture is not symmetric'
    Assert-Contains $symmetry 'counterexample found: yes' 'symmetry witness exists'
    Assert-Contains $symmetry 'list artifact complete: yes' 'symmetry detail artifact is complete'
    Assert-NotContains $symmetry 'Counterexample assignments (' 'large witness is omitted from terminal'
    if ($symmetryFiles.Count -ne 1 -or -not (Test-Path -LiteralPath $symmetryFiles[0])) {
        throw '[symmetry artifact] Expected exactly one existing output file.'
    }
    $symmetryArtifact = Get-Content -LiteralPath $symmetryFiles[0] -Raw
    Assert-Contains $symmetryArtifact 'Counterexample assignments (' 'symmetry artifact has assignments'
    Assert-Contains $symmetryArtifact 'Target values before swap (A=0, B=1)' 'symmetry artifact has before values'
    Assert-Contains $symmetryArtifact 'Target values after swap (A=1, B=0)' 'symmetry artifact has after values'
    Assert-Contains $symmetryArtifact 'Complete: yes' 'symmetry artifact footer is complete'

    $expression = Invoke-Tools @($read, 'func_query boolean_expression y')
    $expressionFiles = @(Get-OutputFiles $expression)
    $createdArtifacts += $expressionFiles
    Assert-Contains $expression 'status: ok' 'Boolean expression query succeeds'
    Assert-Contains $expression 'expression artifact complete: yes' 'equation artifact is complete'
    Assert-NotContains $expression 'list artifact format:' 'Boolean expression skips generic list artifact'
    if ($expressionFiles.Count -ne 1) {
        throw "[Boolean expression artifact] Expected one output_file, got $($expressionFiles.Count)."
    }
    Assert-Contains (Get-Content -LiteralPath $expressionFiles[0] -Raw) 'Complete: yes' 'equation artifact footer is complete'

    Write-Host '[PASS] FunctionReport large-output regression completed'
}
finally {
    foreach ($path in ($createdArtifacts | Select-Object -Unique)) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            Remove-Item -LiteralPath $path -Force
        }
    }
    if (Test-Path -LiteralPath $circuit) {
        Remove-Item -LiteralPath $circuit -Force
    }
}
