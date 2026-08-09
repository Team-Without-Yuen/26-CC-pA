param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$artifact = "Testing/path_compact_test35.txt"
if (Test-Path -LiteralPath $artifact) {
    Remove-Item -LiteralPath $artifact -Force
}

$commands = @(
    "read mini test/test35/path_compact_circuit.v"
    "path_query enumerate net:a net:y -out $artifact -time_limit 10"
    "path_query enumerate net:a net:y -count_only -time_limit 10"
    "exit"
) -join "`n"

$output = $commands | & $Executable 2>&1 | Out-String
$responses = @(
    [regex]::Matches($output, "(?s)TOOL_RESULT_BEGIN.*?TOOL_RESULT_END") |
        ForEach-Object { $_.Value }
)
$passed = 0
$failed = 0

function Check-Result {
    param([bool]$Condition, [string]$Name)
    if ($Condition) {
        $script:passed++
        Write-Output "[PASS] $Name"
    } else {
        $script:failed++
        Write-Output "[FAIL] $Name"
    }
}

function ConvertFrom-Base36 {
    param([string]$Value)
    $result = 0
    foreach ($character in $Value.ToLowerInvariant().ToCharArray()) {
        $digit = if ($character -ge '0' -and $character -le '9') {
            [int]$character - [int][char]'0'
        } else {
            [int]$character - [int][char]'a' + 10
        }
        $result = $result * 36 + $digit
    }
    return $result
}

Check-Result ($responses.Count -eq 4) "every command returns one envelope"
if ($responses.Count -ge 4) {
    Check-Result `
        ($responses[1] -match "status: ok" -and
         $responses[1] -match "Total paths: 4" -and
         $responses[1] -match "Complete enumeration: yes" -and
         $responses[1] -match "Wrote paths to file: yes") `
        "full enumeration reports four complete paths"

    Check-Result `
        ($responses[2] -match "status: ok" -and
         $responses[2] -match "Total paths: 4" -and
         $responses[2] -match "Complete enumeration: yes" -and
         $responses[2] -match "Count only: yes") `
        "count-only DP agrees with full enumeration"
}

Check-Result (Test-Path -LiteralPath $artifact) "compact artifact is created"
if (Test-Path -LiteralPath $artifact) {
    $lines = @(Get-Content -LiteralPath $artifact)
    $text = $lines -join "`n"
    $pathLines = @($lines | Where-Object {
        $_ -match '^Path 0: F ' -or $_ -match '^P [0-9a-z]+ '
    })

    Check-Result `
        ($text -match "Format: COMPACT_PATH_V3" -and
         $text -match "prefix/suffix counts apply to the entire S, including token 0" -and
         $text -match "previous S=\[0,0,2,6,7\].*S=\[0,0,3,6,7\]" -and
         $text -match "Expected paths: 4" -and
         $text -match "Written paths: 4" -and
         $text -match "Complete: yes" -and
         $text -match "Timed out: no") `
        "artifact header and footer report a complete exact result"
    Check-Result ($pathLines.Count -eq 4) "artifact contains exactly four path records"

    $netNames = @{}
    $gateOutputs = @{}
    foreach ($line in $lines) {
        if ($line -match '^N\t([0-9a-z]+)\t(.+)$') {
            $netNames[(ConvertFrom-Base36 $matches[1])] = $matches[2]
        } elseif ($line -match '^G\t([0-9a-z]+)\t([0-9a-z]+)\t') {
            $gateOutputs[(ConvertFrom-Base36 $matches[1])] =
                ConvertFrom-Base36 $matches[2]
        }
    }

    $allRecordsReconstruct = $true
    $indices = @()
    $previousTokens = @()
    foreach ($line in $pathLines) {
        $currentTokens = @()
        if ($line -match '^Path 0: F (.+)$') {
            $indices += 0
            $currentTokens = @(
                $matches[1] -split ' ' |
                    ForEach-Object { ConvertFrom-Base36 $_ }
            )
        } elseif ($line -match '^P ([0-9a-z]+) ([0-9a-z]+) ([0-9a-z]+)(?: (.*))?$') {
            $indices += ConvertFrom-Base36 $matches[1]
            $prefixLength = ConvertFrom-Base36 $matches[2]
            $suffixLength = ConvertFrom-Base36 $matches[3]
            if ($prefixLength + $suffixLength -gt $previousTokens.Count) {
                $allRecordsReconstruct = $false
                continue
            }
            if ($prefixLength -gt 0) {
                $currentTokens += $previousTokens[0..($prefixLength - 1)]
            }
            if (-not [string]::IsNullOrWhiteSpace($matches[4])) {
                $currentTokens += @(
                    $matches[4] -split ' ' |
                        ForEach-Object { ConvertFrom-Base36 $_ }
                )
            }
            if ($suffixLength -gt 0) {
                $currentTokens += $previousTokens[
                    ($previousTokens.Count - $suffixLength)..($previousTokens.Count - 1)
                ]
            }
        } else {
            $allRecordsReconstruct = $false
            continue
        }

        if ($currentTokens.Count -lt 2 -or $netNames[$currentTokens[0]] -ne 'a') {
            $allRecordsReconstruct = $false
            continue
        }
        $currentNetId = $currentTokens[0]
        foreach ($gateId in $currentTokens[1..($currentTokens.Count - 1)]) {
            if (-not $gateOutputs.ContainsKey($gateId)) {
                $allRecordsReconstruct = $false
                break
            }
            $currentNetId = $gateOutputs[$gateId]
        }
        if (-not $netNames.ContainsKey($currentNetId) -or
            $netNames[$currentNetId] -ne 'y') {
            $allRecordsReconstruct = $false
        }
        $previousTokens = $currentTokens
    }

    Check-Result `
        ($allRecordsReconstruct -and
         (@($indices | Sort-Object -Unique) -join ',') -eq '0,1,2,3') `
        "every compact record reconstructs from input a to output y"
}

Write-Output "Passed: $passed"
Write-Output "Failed: $failed"
if ($failed -ne 0) {
    exit 1
}
