param(
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $Executable) {
    $Executable = Join-Path $root 'tools.exe'
}
$tools = (Resolve-Path $Executable).Path
$circuit = Join-Path $PSScriptRoot 'path_depth_predicate.v'
$artifact = Join-Path $root 'Testing\path_depth_predicate_test60.txt'

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

function Assert-True([bool] $Condition, [string] $Label) {
    if (-not $Condition) {
        throw "[$Label] assertion failed"
    }
    Write-Host "[PASS] $Label"
}

function Test-Count([string] $Command, [int] $Expected, [string] $Label,
                    [switch] $PassThru) {
    $output = Invoke-Tools @("read $circuit", $Command)
    Assert-Contains $output 'status: ok' "$Label succeeds"
    Assert-Contains $output "Total paths: $Expected" "$Label count"
    Assert-Contains $output 'Complete enumeration: yes' "$Label is complete"
    if ($PassThru) {
        return $output
    }
}

try {
    Remove-Item -LiteralPath $artifact -Force -ErrorAction SilentlyContinue

    # a->y has one depth-3 path and three depth-4 paths.
    Test-Count 'path_query enumerate pi:a po:y -count_only' 4 'baseline'
    $exact3 = Test-Count `
        'path_query enumerate pi:a po:y -depth_eq 3 -count_only' 1 'exact depth 3' `
        -PassThru
    Assert-Contains $exact3 'Minimum accepted depth: 3' 'exact lower bound is reported'
    Assert-Contains $exact3 'Maximum accepted depth: 3' 'exact upper bound is reported'
    Test-Count 'path_query enumerate pi:a po:y -depth_eq 4 -count_only' 3 'exact depth 4'
    Test-Count 'path_query enumerate pi:a po:y -depth_le 3 -count_only' 1 'at most depth 3'
    Test-Count 'path_query enumerate pi:a po:y -depth_ge 4 -count_only' 3 'at least depth 4'
    Test-Count `
        'path_query enumerate pi:a po:y -depth_ge 3 -depth_le 4 -count_only' `
        4 'inclusive depth range'
    Test-Count 'path_query enumerate pi:a po:y -depth_eq 2 -count_only' 0 'zero match'

    # Depth zero, aggregate endpoints, tied-input and sequential boundary.
    Test-Count `
        'path_query enumerate pi:shared po:shared -depth_eq 0 -count_only' `
        1 'depth-zero shared port'
    Test-Count `
        'path_query enumerate pi:shared po:shared -depth_ge 1 -count_only' `
        0 'depth-zero excluded by lower bound'
    Test-Count `
        'path_query enumerate all_pi all_po -depth_ge 0 -count_only' `
        6 'multi-endpoint global count'
    Test-Count `
        'path_query enumerate pi:a po:tied_out -depth_eq 1 -count_only' `
        1 'tied-input gate remains one path'
    Test-Count `
        'path_query enumerate pi:a dff_d:ff -depth_eq 4 -count_only' `
        3 'DFF.D remains an endpoint boundary'

    # Depth predicate composes with required/avoided structural constraints.
    Test-Count `
        'path_query enumerate pi:a po:y -req net:m1 -depth_eq 3 -count_only' `
        1 'required node exact depth 3'
    Test-Count `
        'path_query enumerate pi:a po:y -req net:m1 -depth_eq 4 -count_only' `
        1 'required node exact depth 4'
    Test-Count `
        'path_query enumerate pi:a po:y -avoid net:m1 -depth_eq 4 -count_only' `
        2 'avoided node exact depth 4'

    # Literal artifact must be self-contained and contain only matching paths.
    $listed = Invoke-Tools @(
        "read $circuit",
        "path_query enumerate pi:a po:y -depth_eq 4 -out $artifact -max_print 0"
    )
    Assert-Contains $listed 'status: ok' 'filtered artifact query succeeds'
    Assert-True (Test-Path -LiteralPath $artifact) 'filtered artifact exists'
    $artifactLines = @(Get-Content -LiteralPath $artifact)
    $artifactText = $artifactLines -join "`n"
    $records = @($artifactLines | Where-Object { $_ -match '^Path [0-9]+: ' })
    Assert-Contains $artifactText 'Depth predicate: exactly 4' 'artifact states predicate'
    Assert-Contains $artifactText 'Total paths: 3' 'artifact states total count'
    Assert-Contains $artifactText 'Expected paths: 3' 'artifact states expected count'
    Assert-Contains $artifactText 'Written paths: 3' 'artifact states written count'
    Assert-Contains $artifactText 'Complete: yes' 'artifact is complete'
    Assert-True ($records.Count -eq 3) 'artifact contains exactly three records'
    foreach ($record in $records) {
        Assert-True (([regex]::Matches($record, '\([A-Z]+\)')).Count -eq 4) `
            'every artifact record has exactly four gates'
    }

    # Strict parser and core contract reject malformed or out-of-scope predicates.
    foreach ($case in @(
        @('path_query enumerate pi:a po:y -depth_eq', '-depth_eq requires'),
        @('path_query enumerate pi:a po:y -depth_eq -1', '-depth_eq requires'),
        @('path_query enumerate pi:a po:y -depth_ge 1.5', '-depth_ge requires'),
        @('path_query enumerate pi:a po:y -depth_le 2147483648', '-depth_le requires'),
        @('path_query enumerate pi:a po:y -depth_ge 4 -depth_le 3', 'empty range'),
        @('path_query exists pi:a po:y -depth_eq 3', 'supported by enumerate and ranked modes only')
    )) {
        $output = Invoke-Tools @("read $circuit", $case[0])
        Assert-Contains $output 'status: error' "rejected malformed predicate: $($case[0])"
        Assert-Contains $output $case[1] "predicate diagnostic: $($case[0])"
        if ($output.Contains('Total paths:')) {
            throw "[malformed predicate] rejected input returned a path count: $($case[0])"
        }
        Write-Host "[PASS] rejected input has no path result -- $($case[0])"
    }

    Write-Host '[PASS] PATH-HIDDEN-005 Stage 1 regression completed'
}
finally {
    Remove-Item -LiteralPath $artifact -Force -ErrorAction SilentlyContinue
}
