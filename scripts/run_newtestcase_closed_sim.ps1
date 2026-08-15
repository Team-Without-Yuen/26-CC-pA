param(
    [string]$Executable = ".\tools.exe",
    [string]$InputRoot = ".\NewTestCase",
    [string]$OutputRoot = ".\testcase_toolans",
    [int]$CommandTimeoutSeconds = 300,
    [int]$BasicCommandTimeoutSeconds = 60,
    [int]$StartTest = 1,
    [int]$EndTest = 40,
    [string[]]$TestNames = @(),
    [int]$MaxPrompt = 0,
    [switch]$RouteOnly
)

$ErrorActionPreference = "Stop"

function Clean-Name([string]$value) {
    return $value.Trim().TrimEnd('.', ',', '?')
}

function Resolve-LiteralCommands([string]$prompt, [string]$testName) {
    $p = $prompt.Trim()
    $l = $p.ToLowerInvariant()
    $outFile = "$OutputRoot/$testName/${testName}_out.v"

    if ($l.StartsWith("this is the beginning")) { return @() }
    if ($l -match "please load the design") { return @("read $InputRoot/$testName/$testName.v") }
    if ($l -match "please write the current design") { return @("write $outFile") }
    if ($l -match "count all the gates.*broken down by gate type" -or
        $l -match "compute the total gate count") { return @("structure_query summary") }

    if ($l -match "how many (and|or|not|nand|nor|xor|xnor|buf) gates.*(?:in|inside).*cone of (?:output )?([^? ]+)") {
        return @("cone_query net_fanin $(Clean-Name $matches[2])")
    }

    if ($l -match "how many (buf|and|or|not|nand|nor|xor|xnor) gates (were added|were removed|were eliminated|were merged)" -or
        $l -match "how many dangling gates were removed" -or
        $l -match "how many redundant gates were removed" -or
        $l -match "how many gates were merged as structural duplicates") {
        return @("report_query last_edit")
    }
    if ($l -match "how many (and|or|not|nand|nor|xor|xnor|buf) gates (are now|are currently)" -or
        $l -match "how many (and|or|not|nand|nor|xor|xnor|buf) gates") {
        $type = $matches[1].ToUpperInvariant()
        return @("structure_query count_by_type $type")
    }

    if ($l -match "insert buffers wherever needed.*more than ([0-9]+) loads") {
        return @("edit_apply insert_buffers_for_fanout $($matches[1])")
    }
    if ($l -match "insert buffers on (?:the reset )?signal ([^ ]+).*at most ([0-9]+)") {
        return @("edit_apply insert_buffers_for_net $(Clean-Name $matches[1]) $($matches[2])")
    }
    if ($l -match "insert a buf gate on signal ([^ ]+).*dedicated buffer") {
        return @("edit_apply insert_buffers_on_each_load $(Clean-Name $matches[1])")
    }
    if ($l -match "trim unused wires and gates") { return @("edit_apply trim_dead_logic") }
    if ($l -match "remove all dangling|delete all gates that do not contribute|sweep out dangling|remove floating nodes|prune the netlist of unused gates|eliminate unused logic gates") {
        return @("edit_apply remove_dangling_logic")
    }
    if ($l -match "check if there are any dangling gates.*remove them") { return @("edit_apply remove_dangling_logic") }
    if ($l -match "back-to-back inverter|back-to-back inverters") { return @("edit_apply collapse_double_inverter") }
    if ($l -match "redundant gates.*remove them") { return @("edit_apply merge_structurally_equivalent_gates") }
    if ($l -match "merge any pairs of gates.*structural duplicates") { return @("edit_apply merge_structurally_equivalent_gates") }
    if ($l -match "merge all gate pairs.*functionally equivalent") { return @("edit_apply merge_functionally_equivalent_gates whole") }
    if ($l -match "rename gate ([^ ]+) to ([^ ]+)" -or $l -match "identifier of gate ([^ ]+) to ([^ ]+)") {
        return @("edit_apply rename_gate $(Clean-Name $matches[1]) $(Clean-Name $matches[2])")
    }
    if ($l -match "(?:identifier|name) of (?:wire|signal) ([^ ]+) to ([^ ]+)" -or
        $l -match "rename wire ([^ ]+) to ([^ ]+)" -or
        $l -match "name of signal ([^ ]+) to ([^ ]+)" -or
        $l -match "rename internal signal ([^ ]+) to ([^ ]+)") {
        return @("edit_apply rename_net $(Clean-Name $matches[1]) $(Clean-Name $matches[2])")
    }

    if ($l -match "all gates that now connect to (?:the )?(?:renamed )?signal ([^ ]+)") {
        $net = Clean-Name $matches[1]
        return @("structure_query net_driver $net", "structure_query net_loads $net")
    }

    if ($l -match "reconstruct the entire netlist using only and and not") {
        return @("edit_apply convert_basis whole -allow AND NOT")
    }
    if ($l -match "remap the entire design to use only nand and not") {
        return @("edit_apply convert_basis whole -allow NAND NOT")
    }
    if ($l -match "convert the logic cone of ([^ ]+) to use only nor and not") {
        return @("edit_apply convert_basis net_fanin $(Clean-Name $matches[1]) -allow NOR NOT")
    }
    if ($l -match "restructure the logic cone of (?:output )?([^ ]+) using only nand and not") {
        return @("edit_apply convert_basis net_fanin $(Clean-Name $matches[1]) -allow NAND NOT")
    }
    if ($l -match "replace all 2-input or gates in the cone of ([^ ]+).*(nand and not)") {
        return @("edit_apply replace_type net_fanin $(Clean-Name $matches[1]) OR -allow NAND NOT")
    }
    if ($l -match "decompose all xor gates in the fanin cone of ([^ ]+).*(and, or, and not)") {
        return @("edit_apply replace_type net_fanin $(Clean-Name $matches[1]) XOR -allow AND OR NOT")
    }
    if ($l -match "replace all xnor gates.*nor-only|convert every xnor gate.*nor-only") {
        return @("edit_apply replace_type whole XNOR -allow NOR")
    }
    if ($l -match "replace all xor gates.*nand-only|convert every xor gate.*4-nand") {
        return @("edit_apply replace_type whole XOR -allow NAND")
    }
    if ($l -match "replace all 2-input nand gates.*constant 1.*inverters") {
        return @("edit_apply simplify_constants NAND 1 --inputs 2")
    }
    if ($l -match "simplify the reported (and|or|nand|nor) gates.*constant ([01])") {
        return @("edit_apply simplify_constants $($matches[1].ToUpperInvariant()) $($matches[2])")
    }
    if ($l -match "simplify the reported (and|or|nand|nor) gates.*constant") {
        return @("edit_apply simplify_constants $($matches[1].ToUpperInvariant()) any")
    }

    if ($l -match "optimi[sz]e.*cone of (?:output )?([^ ]+).*nand and not" -or
        $l -match "depth optimization.*cone of (?:output )?([^ ]+).*nand and not") {
        $scopeName = Clean-Name $matches[1]
        $objective = if ($l -match "maximum logic depth of the final design|maximum path depth of the final design") { "global" } else { "cone" }
        return @("opt_apply critical_path_depth --scope net_fanin $scopeName --objective $objective --allowed NAND NOT")
    }
    if ($l -match "optimi[sz]e.*cone of ([^ ]+).*and, or, and not") {
        return @("opt_apply critical_path_depth --scope net_fanin $(Clean-Name $matches[1]) --objective global --allowed AND OR NOT")
    }
    if ($l -match "critical path.*cone of ([^ ]+).*nor and not") {
        return @("opt_apply critical_path_depth --scope net_fanin $(Clean-Name $matches[1]) --objective global --allowed NOR NOT")
    }
    if ($l -match "minimi[sz]e.*netlist remains and and not|maximum logic depth.*and and not") {
        return @("opt_apply critical_path_depth --scope whole --objective global --allowed AND NOT")
    }
    if ($l -match "reduce the critical path|optimi[sz]e the logic to minimi[sz]e maximum path depth|minimi[sz]e the critical path") {
        return @("opt_apply critical_path_depth --scope whole --objective global")
    }

    if ($l -match "prove that the transformed design.*pre-transformation") { return @("equiv_query previous_edit") }
    if ($l -match "current design and the original|current netlist.*last loaded|still functionally equivalent to the original|current design and the original") {
        return @("equiv_query original")
    }

    if ($l -match "report any (and|or|nand|nor) gates with (?:a )?constant ([01]) input") {
        return @("structure_query const_input_gates $($matches[1].ToUpperInvariant()) $($matches[2])")
    }
    if ($l -match "report any (nand|nor) gates with constant inputs") {
        return @("structure_query const_input_gates $($matches[1].ToUpperInvariant()) any")
    }
    if ($l -match "list all gates with one or more inputs tied to 1'b1") {
        return @("structure_query const_input_gates all 1")
    }
    if ($l -match "list all (and|or|not|nand|nor|xor|xnor|buf|dff) gates") {
        return @("structure_query gates_by_type $($matches[1].ToUpperInvariant())")
    }
    if ($l -match "what type of gate is ([^? ]+)") {
        $g = Clean-Name $matches[1]
        return @("structure_query gate_info $g", "structure_query gate_inputs $g", "structure_query gate_output $g")
    }
    if ($l -match "report every gate connected to the output of ([^ ]+)|immediate successors of gate ([^ ]+)|number of gates driven by ([^ ]+)") {
        $g = @($matches[1],$matches[2],$matches[3]) | Where-Object {$_} | Select-Object -First 1
        return @("structure_query gate_fanout $(Clean-Name $g)")
    }
    if ($l -match "fanout of primary input ([^? ]+).*(drives directly)") {
        return @("structure_query net_loads $(Clean-Name $matches[1])")
    }
    if ($l -match "which primary input has the highest fanout") { return @("structure_query pi_fanout") }
    if ($l -match "maximum fanout of ([^ ]+) now") { return @("structure_query fanout_load $(Clean-Name $matches[1])") }
    if ($l -match "number of primary inputs and outputs|how many primary inputs and primary outputs") { return @("structure_query summary") }
    if ($l -match "list all (?:the )?primary inputs.*bit widths") { return @("structure_query list_pi") }
    if ($l -match "list all (?:the )?primary outputs.*bit widths") { return @("structure_query list_po") }
    if ($l -match "floating inputs or unconnected output ports") { return @("structure_query structural_issues") }
    if ($l -match "how many floating signals") { return @("structure_query structural_issues") }
    if ($l -match "list all flip-flops driven by clock ([^ ]+)") { return @("structure_query fanout_load $(Clean-Name $matches[1])") }

    if ($l -match "gates shared between the fanin cones of ([^ ]+) and ([^ ]+)") {
        return @("cone_query shared_fanin $(Clean-Name $matches[1]) $(Clean-Name $matches[2])")
    }
    if ($l -match "which (?:primary )?output has the largest fanin (?:logic )?cone") {
        return @("cone_query largest_output")
    }
    if ($l -match "number of each gate type in (?:the )?(?:logic )?cone of (?:output )?([^ ]+)|how many gates are in the logic cone of (?:output )?([^ ]+)|how many gates are in the fanin cone of primary output ([^ ]+)") {
        $n = @($matches[1],$matches[2],$matches[3]) | Where-Object {$_} | Select-Object -First 1
        return @("cone_query net_fanin $(Clean-Name $n)")
    }
    if ($l -match "fanin logic cone of output ([^ ]+)|transitive fanin cone of output ([^ ]+)") {
        $n = @($matches[1],$matches[2]) | Where-Object {$_} | Select-Object -First 1
        return @("cone_query net_fanin $(Clean-Name $n)")
    }
    if ($l -match "transitive fanout.*input ([^ ]+)|all gates reachable from ([^ ]+)|determine all gates reachable from ([^ ]+)") {
        $n = @($matches[1],$matches[2],$matches[3]) | Where-Object {$_} | Select-Object -First 1
        return @("cone_query net_fanout $(Clean-Name $n)")
    }
    if ($l -match "which output \(([^ )]+) or ([^) ]+)\) has the largest fanin logic cone") {
        return @(
            "cone_query net_fanin $(Clean-Name $matches[1])",
            "cone_query net_fanin $(Clean-Name $matches[2])"
        )
    }
    if ($l -match "which output has the largest fanin cone") { return @("cone_query largest_output") }

    if ($l -match "path.*from (?:primary input |input )?([^ ]+) to (?:primary output |output )?([^ ]+).*avoid(?:s)?(?: node)? ([^ ]+)") {
        return @("path_query exists net:$(Clean-Name $matches[1]) net:$(Clean-Name $matches[2]) -avoid net:$(Clean-Name $matches[3])")
    }
    if ($l -match "path from (?:primary input |input )?([^ ]+) to (?:primary output |output )?([^ ]+) exists that does not traverse node ([^ ]+)") {
        return @("path_query exists net:$(Clean-Name $matches[1]) net:$(Clean-Name $matches[2]) -avoid net:$(Clean-Name $matches[3])")
    }
    if ($l -match "path connecting input ([^ ]+) to output ([^ ]+).*avoiding ([^ ]+)") {
        return @("path_query exists net:$(Clean-Name $matches[1]) net:$(Clean-Name $matches[2]) -avoid net:$(Clean-Name $matches[3])")
    }
    if ($l -match "combinational path (?:exist )?from primary input ([^ ]+) to primary output ([^? ]+)") {
        return @("path_query exists pi:$(Clean-Name $matches[1]) po:$(Clean-Name $matches[2])")
    }
    if ($l -match "(?:does )?(?:a )?combinational path (?:exist )?from (?:primary input |input )?([^ ]+) to (?:primary output |output )?([^? ]+).*(?:exist|yes or no)") {
        return @("path_query exists net:$(Clean-Name $matches[1]) net:$(Clean-Name $matches[2])")
    }
    if ($l -match "every path from input ([^ ]+) to output ([^ ]+) pass through gate ([^ ]+)") {
        return @("path_query every_through net:$(Clean-Name $matches[1]) net:$(Clean-Name $matches[2]) -req gate:$(Clean-Name $matches[3])")
    }
    if ($l -match "all paths of length 0") { return @("path_query direct_pi_po") }
    if ($l -match "wire ([^ ]+) is a cut between any primary input and any primary output") {
        return @("path_query pi_po_cut $(Clean-Name $matches[1])")
    }
    if ($l -match "articulation points.*between ([^ ]+) and ([^ ]+)") {
        return @("path_query articulation_between net:$(Clean-Name $matches[1]) net:$(Clean-Name $matches[2])")
    }
    if ($l -match "list every path originating at primary input ([^ ]+) and terminating at primary output ([^ ]+)|all combinational paths from primary input ([^ ]+) to primary output ([^ ]+)") {
        $a = @($matches[1],$matches[3]) | Where-Object {$_} | Select-Object -First 1
        $b = @($matches[2],$matches[4]) | Where-Object {$_} | Select-Object -First 1
        return @("path_query enumerate net:$(Clean-Name $a) net:$(Clean-Name $b)")
    }
    if ($l -match "complete enumeration of paths between ([^ ]+) and ([^ ]+)") {
        return @("path_query enumerate net:$(Clean-Name $matches[1]) net:$(Clean-Name $matches[2])")
    }
    if ($l -match "maximum logic depth from input ([^ ]+) to output ([^ ]+)|longest combinational path depth from ([^ ]+) to ([^ ]+)|critical path depth between ([^ ]+) and ([^ ]+)") {
        $a = @($matches[1],$matches[3],$matches[5]) | Where-Object {$_} | Select-Object -First 1
        $b = @($matches[2],$matches[4],$matches[6]) | Where-Object {$_} | Select-Object -First 1
        return @("path_query max_depth net:$(Clean-Name $a) net:$(Clean-Name $b)")
    }
    if ($l -match "register-to-register paths") { return @("path_query enumerate all_dff_q all_dff_d") }
    if ($l -match "maximum combinational (?:logic )?depth on any register-to-register path") { return @("path_query max_depth all_dff_q all_dff_d") }
    if ($l -match "maximum logic depth from any primary input to any dff d-pin") { return @("path_query max_depth all_pi all_dff_d") }

    if ($l -match "maximum logic depth of the fanin cone of output ([^ ]+)|depth of the cone of ([^ ]+) now") {
        $n = @($matches[1],$matches[2]) | Where-Object {$_} | Select-Object -First 1
        return @("depth_query net $(Clean-Name $n)")
    }
    if ($l -match "outputs have a logic depth greater than ([0-9]+)") { return @("depth_query po_exceeding $($matches[1])") }
    if ($l -match "gate ([^ ]+) lies on any maximum-depth path") { return @("depth_query gate_on_critical $(Clean-Name $matches[1])") }
    if ($l -match "which output bit has the deepest|which output has the deepest") { return @("depth_query deepest_output") }
    if ($l -match "maximum combinational depth from any primary input to any primary output") { return @("path_query max_depth all_pi all_po") }
    if ($l -match "maximum combinational logic depth|maximum logic depth in the design now") { return @("depth_query global_critical") }

    if ($l -match "signals ([^ ]+) and ([^ ]+).*functionally equivalent|functional equivalence between internal signals ([^ ]+) and ([^ ]+)|([^ ]+) and ([^ ]+) produce identical logic values") {
        $a = @($matches[1],$matches[3],$matches[5]) | Where-Object {$_} | Select-Object -First 1
        $b = @($matches[2],$matches[4],$matches[6]) | Where-Object {$_} | Select-Object -First 1
        return @("func_query equivalence $(Clean-Name $a) $(Clean-Name $b)")
    }
    if ($l -match "output ([^ ]+) always 0") { return @("func_query always_zero $(Clean-Name $matches[1])") }
    if ($l -match "output ([^ ]+) depend on input ([^ ]+)") { return @("func_query depends_on $(Clean-Name $matches[1]) $(Clean-Name $matches[2])") }
    if ($l -match "function at ([^ ]+) is symmetric with respect to inputs ([^ ]+) and ([^ ]+)") {
        return @("func_query symmetry $(Clean-Name $matches[1]) $(Clean-Name $matches[2]) $(Clean-Name $matches[3])")
    }
    if ($l -match "boolean equation for output ([^ ]+)|boolean function does output ([^ ]+) compute|logic expression for ([^ ]+)") {
        $n = @($matches[1],$matches[2],$matches[3]) | Where-Object {$_} | Select-Object -First 1
        return @("func_query boolean_expression $(Clean-Name $n)")
    }
    if ($l -match "pair of internal signals.*nand\(a, b\).*equivalent to ([^ ]+)") {
        return @("func_search nand_pair $(Clean-Name $matches[1])")
    }

    if ($l -match "d input logic of the flip-flops") { return @("sequential_query enable_hold all") }
    if ($l -match "how many flip-flops.*enable or hold") { return @("sequential_query enable_hold all --summary-only") }

    return $null
}

function Start-ToolProcess([string]$exe) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = (Resolve-Path -LiteralPath $exe).Path
    $psi.WorkingDirectory = (Get-Location).Path
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    [void]$process.Start()
    return $process
}

function Invoke-ToolCommand($process, [string]$command, [int]$timeoutSeconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process.StandardInput.WriteLine($command)
    $process.StandardInput.Flush()
    $lines = [Collections.Generic.List[string]]::new()
    $complete = $false
    while ($watch.Elapsed.TotalSeconds -lt $timeoutSeconds) {
        $remaining = [Math]::Max(1, [int](($timeoutSeconds - $watch.Elapsed.TotalSeconds) * 1000))
        $task = $process.StandardOutput.ReadLineAsync()
        if (-not $task.Wait($remaining)) { break }
        $line = $task.Result
        if ($null -eq $line) { break }
        $lines.Add($line)
        if ($line -match "TOOL_RESULT_END") { $complete = $true; break }
    }
    $watch.Stop()
    $text = $lines -join "`n"
    $status = if ($text -match "(?m)^status:\s*([^\r\n]+)") { $matches[1].Trim() } else { "unknown" }
    return [PSCustomObject]@{
        Complete = $complete
        TimedOut = -not $complete
        Status = $status
        Elapsed = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        Output = $text
    }
}

function Summarize-ToolOutput([string]$output) {
    $lines = $output -split "`r?`n" | Where-Object {
        $_ -notmatch "^(eda> )?TOOL_RESULT_(BEGIN|END)$" -and
        $_ -notmatch "^(ok|command|mode|design_revision|complete):"
    }
    return ($lines -join "`n").Trim()
}

function Test-IsStateCommand([string]$command) {
    return $command -match "^(read|edit_apply|opt_apply|constraint_set|constraint_clear)\b"
}

function Get-CommandTimeoutSeconds([string]$command) {
    if ($command -match "^(read|write)\b") {
        return $BasicCommandTimeoutSeconds
    }
    return $CommandTimeoutSeconds
}

function Restore-ToolSession([string]$exe, $history, [int]$timeoutSeconds) {
    $restored = Start-ToolProcess $exe
    foreach ($command in $history) {
        $replayTimeoutSeconds = if ($command -match "^(read|write)\b") {
            $BasicCommandTimeoutSeconds
        } else {
            $timeoutSeconds
        }
        $result = Invoke-ToolCommand $restored $command $replayTimeoutSeconds
        if (-not $result.Complete -or $result.Status -eq "error") {
            try { $restored.Kill($true) } catch {}
            throw "Failed to restore tool session with command: $command"
        }
    }
    return $restored
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$summary = [Collections.Generic.List[object]]::new()

foreach ($testDir in Get-ChildItem -LiteralPath $InputRoot -Directory | Sort-Object Name) {
    $testName = $testDir.Name
    if ($testName -notmatch '^test(\d+)$') { continue }
    $testNumber = [int]$matches[1]
    if ($testNumber -lt $StartTest -or $testNumber -gt $EndTest) { continue }
    if ($TestNames.Count -gt 0 -and $testName -notin $TestNames) { continue }
    $promptPath = Join-Path $testDir.FullName "prompt.txt"
    if (-not (Test-Path -LiteralPath $promptPath)) { continue }
    $resultDir = Join-Path $OutputRoot $testName
    New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
    $answerLines = [Collections.Generic.List[string]]::new()
    $callLines = [Collections.Generic.List[string]]::new()
    $unresolved = 0
    $timeouts = 0
    $toolTimeouts = 0
    $process = if ($RouteOnly) { $null } else { Start-ToolProcess $Executable }
    $stateHistory = [Collections.Generic.List[string]]::new()
    $index = 0
    foreach ($prompt in Get-Content -LiteralPath $promptPath) {
        if ($MaxPrompt -gt 0 -and $index -ge $MaxPrompt) { break }
        ++$index
        $answerLines.Add("#RESPONSE $index")
        $callLines.Add("#$index")
        $callLines.Add("Prompt: $prompt")
        if ($prompt.ToLowerInvariant().StartsWith("this is the beginning")) {
            $answerLines.Add("The testcase $testName has started.")
            $callLines.Add("Function: none")
            $callLines.Add("Reason: testcase start marker")
        } else {
            $commands = @(Resolve-LiteralCommands $prompt $testName)
            if ($commands.Count -eq 0 -or ($commands.Count -eq 1 -and $null -eq $commands[0])) {
                ++$unresolved
                $answerLines.Add("UNRESOLVED: the closed-simulation tool guide did not provide a confident command mapping.")
                $callLines.Add("Function: none")
                $callLines.Add("Result status: unresolved")
            } else {
                $callNumber = 0
                foreach ($command in $commands) {
                    ++$callNumber
                    if ($RouteOnly) {
                        $callLines.Add("Call ${callNumber}:")
                        $callLines.Add("Raw command: $command")
                        $callLines.Add("Result status: routed_not_executed")
                        $answerLines.Add("ROUTED: $command")
                        continue
                    }
                    $effectiveTimeoutSeconds = Get-CommandTimeoutSeconds $command
                    $result = Invoke-ToolCommand $process $command $effectiveTimeoutSeconds
                    $callLines.Add("Call ${callNumber}:")
                    $callLines.Add("Raw command: $command")
                    $callLines.Add("Result status: $($result.Status)")
                    $callLines.Add("Complete: $($result.Complete.ToString().ToLowerInvariant())")
                    $callLines.Add("Elapsed: $($result.Elapsed)")
                    $callLines.Add("Time limit: $effectiveTimeoutSeconds")
                    if ($result.Status -eq "timeout") {
                        ++$toolTimeouts
                    }
                    if ($result.TimedOut) {
                        ++$timeouts
                        $answerLines.Add("TIMEOUT: command was terminated after $effectiveTimeoutSeconds seconds.")
                        $callLines.Add("Timeout interrupted")
                        try { $process.Kill($true) } catch {}
                        $process = Restore-ToolSession $Executable $stateHistory $CommandTimeoutSeconds
                        break
                    }
                    if (Test-IsStateCommand $command) {
                        $stateHistory.Add($command)
                    }
                    $answerLines.Add((Summarize-ToolOutput $result.Output))
                }
            }
        }
        $answerLines.Add("#END $index")
        $answerLines.Add("")
        $callLines.Add("")
        if ($null -ne $process -and $process.HasExited) {
            $process = Restore-ToolSession $Executable $stateHistory $CommandTimeoutSeconds
        }
    }
    if ($null -ne $process -and -not $process.HasExited) {
        try { $process.StandardInput.WriteLine("quit"); $process.StandardInput.Flush() } catch {}
        if (-not $process.WaitForExit(2000)) { try { $process.Kill($true) } catch {} }
    }
    Set-Content -LiteralPath (Join-Path $resultDir "${testName}_ans") -Value $answerLines -Encoding utf8
    Set-Content -LiteralPath (Join-Path $resultDir "tool_calls.txt") -Value $callLines -Encoding utf8
    $mode = if ($RouteOnly) { "route_only" } else { "execute" }
    $summary.Add([PSCustomObject]@{Test=$testName;Mode=$mode;Prompts=$index;Unresolved=$unresolved;RunnerTimeouts=$timeouts;ToolTimeouts=$toolTimeouts})
    Write-Output "[$testName] mode=$mode prompts=$index unresolved=$unresolved runner_timeouts=$timeouts tool_timeouts=$toolTimeouts"
}

$summary | ConvertTo-Csv -NoTypeInformation | Set-Content -LiteralPath (Join-Path $OutputRoot "summary.csv") -Encoding utf8
$summary
