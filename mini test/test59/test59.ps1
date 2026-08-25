param(
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $Executable) {
    $Executable = Join-Path $root 'tools.exe'
}
$tools = (Resolve-Path $Executable).Path
$circuit = Join-Path $PSScriptRoot 'path_deadline.v'
$artifact = Join-Path $root 'Testing\path_deadline_test59.txt'
$requiredArtifact = Join-Path $root 'Testing\path_deadline_required_test59.txt'
$filteredArtifact = Join-Path $root 'Testing\path_deadline_filtered_test59.txt'

function Invoke-ToolsTimed([string[]] $Commands) {
    $inputText = (($Commands + 'exit') -join "`n") + "`n"
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $text = ($inputText | & $tools 2>&1) -join "`n"
    $watch.Stop()
    return [pscustomobject]@{
        Text = $text
        Seconds = $watch.Elapsed.TotalSeconds
    }
}

function Assert-True([bool] $Condition, [string] $Label) {
    if (-not $Condition) {
        throw "[$Label] assertion failed"
    }
    Write-Host "[PASS] $Label"
}

function Assert-Contains([string] $Text, [string] $Expected, [string] $Label) {
    if (-not $Text.Contains($Expected)) {
        throw "[$Label] Missing expected text: $Expected`n--- output ---`n$Text"
    }
    Write-Host "[PASS] $Label"
}

function Test-PartialArtifact([string] $Path, [string] $Label) {
    Assert-True (Test-Path -LiteralPath $Path) "$Label artifact exists"
    $lines = @(Get-Content -LiteralPath $Path)
    $text = $lines -join "`n"
    $records = @($lines | Where-Object { $_ -match '^Path [0-9]+: ' })
    $writtenMatch = [regex]::Match($text, '(?m)^Written paths: ([0-9]+)$')
    Assert-True $writtenMatch.Success "$Label footer has written count"
    Assert-True ([uint64]$writtenMatch.Groups[1].Value -eq [uint64]$records.Count) `
        "$Label written count matches records"
    Assert-Contains $text 'Format: LITERAL_PATH_V1' "$Label keeps literal format"
    Assert-Contains $text 'Complete: no' "$Label artifact is incomplete"
    Assert-Contains $text 'Timed out: yes' "$Label artifact records timeout"
}

try {
    foreach ($path in @($artifact, $requiredArtifact, $filteredArtifact)) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }

    $read = "read $circuit"
    $count = Invoke-ToolsTimed @(
        $read,
        'path_query enumerate net:a net:y -count_only -time_limit 2'
    )
    Assert-Contains $count.Text 'status: ok' 'count-only query succeeds'
    Assert-Contains $count.Text 'Total paths: 16777216' 'count-only query is exact'
    Assert-Contains $count.Text 'Complete enumeration: yes' 'count-only query is complete'

    $enumerate = Invoke-ToolsTimed @(
        $read,
        "path_query enumerate net:a net:y -out $artifact -max_print 0 -time_limit 0.02"
    )
    Assert-Contains $enumerate.Text 'status: timeout' 'unconstrained result has timeout status'
    Assert-Contains $enumerate.Text 'complete: false' 'unconstrained envelope is incomplete'
    Assert-Contains $enumerate.Text 'Complete enumeration: no' 'unconstrained result is incomplete'
    Assert-Contains $enumerate.Text 'Timed out: yes' 'unconstrained result reports timeout'
    Assert-True ($enumerate.Seconds -lt 1.0) 'unconstrained request stays within one budget plus tolerance'
    Test-PartialArtifact $artifact 'unconstrained'

    $required = Invoke-ToolsTimed @(
        $read,
        "path_query enumerate net:a net:y -req net:m12 -out $requiredArtifact -max_print 0 -time_limit 0.02"
    )
    Assert-Contains $required.Text 'status: timeout' 'required-node result has timeout status'
    Assert-Contains $required.Text 'complete: false' 'required-node envelope is incomplete'
    Assert-Contains $required.Text 'Complete enumeration: no' 'required-node result is incomplete'
    Assert-Contains $required.Text 'Timed out: yes' 'required-node result reports timeout'
    Assert-True ($required.Seconds -lt 1.0) 'required-node request stays within one budget plus tolerance'
    Test-PartialArtifact $requiredArtifact 'required-node'

    $filtered = Invoke-ToolsTimed @(
        $read,
        "path_query enumerate net:a net:y -depth_eq 49 -out $filteredArtifact -max_print 0 -time_limit 0.02"
    )
    Assert-Contains $filtered.Text 'status: timeout' 'depth-filtered result has timeout status'
    Assert-Contains $filtered.Text 'complete: false' 'depth-filtered envelope is incomplete'
    Assert-Contains $filtered.Text 'Minimum accepted depth: 49' 'depth-filtered lower bound is reported'
    Assert-Contains $filtered.Text 'Maximum accepted depth: 49' 'depth-filtered upper bound is reported'
    Assert-True ($filtered.Seconds -lt 1.0) 'depth-filtered request stays within one budget plus tolerance'
    Test-PartialArtifact $filteredArtifact 'depth-filtered'
    Assert-Contains ((Get-Content -LiteralPath $filteredArtifact -Raw)) `
        'Depth predicate: exactly 49' 'depth-filtered artifact states predicate'

    Write-Host '[PASS] PATH-DEADLINE-001 regression completed'
}
finally {
    foreach ($path in @($artifact, $requiredArtifact, $filteredArtifact)) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}
