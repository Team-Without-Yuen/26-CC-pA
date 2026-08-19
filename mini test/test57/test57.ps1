$ErrorActionPreference = 'Stop'

# func_query strict-parser regression (FUNCTION-PARSER-001).

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$tools = Join-Path $root 'tools.exe'
$circuit = Join-Path $PSScriptRoot 'function_parser.v'
$createdArtifacts = @()

function Invoke-Tools([string[]] $Commands) {
    $inputText = (($Commands + 'exit') -join "`n") + "`n"
    $output = ($inputText | & $tools 2>&1) -join "`n"
    $files = [regex]::Matches(($output -replace "`r", ''), '(?m)^  output_file: (.+)$') |
        ForEach-Object { $_.Groups[1].Value.Trim() }
    $script:createdArtifacts += $files
    return $output
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

function Assert-ParserError([string] $Command, [string] $Expected, [string] $Label) {
    $result = Invoke-Tools @($script:read, $Command)
    Assert-Contains $result 'status: error' "$Label rejected"
    Assert-Contains $result 'complete: false' "$Label is incomplete"
    Assert-Contains $result $Expected "$Label diagnostic"
    Assert-NotContains $result 'Unknown func_query mode' "$Label keeps recognized mode"
}

try {
    $script:read = "read $circuit"

    $validCases = @(
        @{ Command = 'func_query equivalence y y'; Label = 'equivalence' },
        @{ Command = 'func_query conditional_equivalence y y en 0'; Label = 'conditional_equivalence' },
        @{ Command = 'func_query equivalence_when y y en 1'; Label = 'equivalence_when alias' },
        @{ Command = 'func_query can_be_value y 1'; Label = 'can_be_value' },
        @{ Command = 'func_query constant y 0'; Label = 'constant' },
        @{ Command = 'func_query always_zero y'; Label = 'always_zero' },
        @{ Command = 'func_query always_one y'; Label = 'always_one' },
        @{ Command = 'func_query truth_status y'; Label = 'truth_status' },
        @{ Command = 'func_query depends_on y a'; Label = 'depends_on' },
        @{ Command = 'func_query functional_dependence y a'; Label = 'functional_dependence alias' },
        @{ Command = 'func_query symmetry y a b'; Label = 'symmetry' },
        @{ Command = 'func_query symmetric y a b'; Label = 'symmetric alias' },
        @{ Command = 'func_query boolean_expression y'; Label = 'boolean_expression' },
        @{ Command = 'func_query expression y'; Label = 'expression alias' },
        @{ Command = 'func_query simplified_expression y 2'; Label = 'simplified_expression' },
        @{ Command = 'func_query support_pi y'; Label = 'support_pi' },
        @{ Command = 'func_query primary_inputs_of_net y'; Label = 'primary_inputs_of_net alias' }
    )

    foreach ($case in $validCases) {
        $result = Invoke-Tools @($script:read, $case.Command)
        Assert-Contains $result 'status: ok' "$($case.Label) succeeds"
        Assert-Contains $result 'complete: true' "$($case.Label) is complete"
    }

    $arityCases = @(
        @{ Command = 'func_query equivalence y'; Error = 'equivalence requires <net_a> <net_b>'; Label = 'equivalence missing net' },
        @{ Command = 'func_query equivalence y y stray'; Error = 'Unexpected func_query argument: stray'; Label = 'equivalence trailing arg' },
        @{ Command = 'func_query conditional_equivalence y y en'; Error = 'conditional_equivalence requires <net_a> <net_b> <condition_net> <0|1>'; Label = 'conditional missing value' },
        @{ Command = 'func_query can_be_value'; Error = 'can_be_value requires <net> <0|1>'; Label = 'can_be_value missing args' },
        @{ Command = 'func_query constant y 0 stray'; Error = 'Unexpected func_query argument: stray'; Label = 'constant trailing arg' },
        @{ Command = 'func_query always_zero'; Error = 'always_zero requires <net>'; Label = 'always_zero missing net' },
        @{ Command = 'func_query always_one y stray'; Error = 'Unexpected func_query argument: stray'; Label = 'always_one trailing arg' },
        @{ Command = 'func_query truth_status --bad'; Error = 'truth_status requires <net>'; Label = 'truth_status option as net' },
        @{ Command = 'func_query depends_on y'; Error = 'depends_on requires <target_net> <input_net>'; Label = 'depends_on missing input' },
        @{ Command = 'func_query symmetry y a'; Error = 'symmetry requires <target_net_or_bus> <input_a> <input_b>'; Label = 'symmetry missing input' },
        @{ Command = 'func_query symmetry y a b stray'; Error = 'Unexpected func_query argument: stray'; Label = 'symmetry trailing arg' },
        @{ Command = 'func_query boolean_expression'; Error = 'boolean_expression requires <net>'; Label = 'expression missing net' },
        @{ Command = 'func_query simplified_expression y'; Error = 'simplified_expression requires <net> <non_negative_max_depth>'; Label = 'simplified missing depth' },
        @{ Command = 'func_query support_pi y stray'; Error = 'Unexpected func_query argument: stray'; Label = 'support trailing arg' }
    )
    foreach ($case in $arityCases) {
        Assert-ParserError $case.Command $case.Error $case.Label
    }

    $valueCases = @(
        @{ Command = 'func_query conditional_equivalence y y en 2'; Error = 'conditional_equivalence requires condition value 0 or 1'; Label = 'condition value 2' },
        @{ Command = 'func_query conditional_equivalence y y en -1'; Error = 'conditional_equivalence requires condition value 0 or 1'; Label = 'negative condition value' },
        @{ Command = 'func_query conditional_equivalence y y en abc'; Error = 'conditional_equivalence requires condition value 0 or 1'; Label = 'text condition value' },
        @{ Command = 'func_query can_be_value y 1.0'; Error = 'can_be_value requires value 0 or 1'; Label = 'decimal can-be value' },
        @{ Command = 'func_query can_be_value y 2147483648'; Error = 'can_be_value requires value 0 or 1'; Label = 'overflow can-be value' },
        @{ Command = 'func_query constant y -1'; Error = 'constant requires value 0 or 1'; Label = 'negative constant value' },
        @{ Command = 'func_query constant y abc'; Error = 'constant requires value 0 or 1'; Label = 'text constant value' },
        @{ Command = 'func_query simplified_expression y -1'; Error = 'simplified_expression requires a non-negative integer max depth'; Label = 'negative max depth' },
        @{ Command = 'func_query simplified_expression y 1.5'; Error = 'simplified_expression requires a non-negative integer max depth'; Label = 'decimal max depth' },
        @{ Command = 'func_query simplified_expression y abc'; Error = 'simplified_expression requires a non-negative integer max depth'; Label = 'text max depth' },
        @{ Command = 'func_query simplified_expression y 2147483648'; Error = 'simplified_expression requires a non-negative integer max depth'; Label = 'overflow max depth' }
    )
    foreach ($case in $valueCases) {
        Assert-ParserError $case.Command $case.Error $case.Label
    }

    $zeroDepth = Invoke-Tools @($script:read, 'func_query simplified_expression y 0')
    Assert-Contains $zeroDepth 'status: ok' 'zero max depth is valid'

    $unknown = Invoke-Tools @($script:read, 'func_query definitely_not_a_mode y')
    Assert-Contains $unknown 'Unknown func_query mode: definitely_not_a_mode' 'unknown mode remains distinct'

    Write-Host '[PASS] FUNCTION-PARSER-001 strict parser regression completed'
}
finally {
    foreach ($path in ($createdArtifacts | Select-Object -Unique)) {
        if ($path -and (Test-Path -LiteralPath $path)) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}
