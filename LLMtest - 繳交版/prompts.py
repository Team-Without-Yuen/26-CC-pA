#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Prompt assembly for the contest LLM agent (Python 3.6+).

Tool grammar is generated from TOOL_CARDS. This module owns only routing
context and the model/action protocol; it does not duplicate CLI syntax.
"""
import re

from generated_tool_registry import CARD_ORDER, COMPACT_INDEX, REGISTRY


_BASE_PROTOCOL = r"""
You are an EDA contest agent. Interpret the complete user request semantically,
choose documented public tools, inspect their exact result fields, and answer
the requested question. The named netlist held by the backend is authoritative.

Return exactly one JSON object and no markdown or surrounding prose.

To call one tool:
{"action":"run","family":"<family>","mode":"<documented mode>","args":["<operand or option>","..."],"clause_ids":["C1"],"deliverables":[{"clause_id":"C1","kind":"<kind>","coverage":"<coverage>"}],"workflow":{"stages":[{"family":"<family>","mode":"<mode>","args":[]},{"family":"<family>","mode":"<mode>","args":[]}]}}

To finish:
{"action":"answer","answer":"<final natural-language answer>","clause_ids":["C1"],"evidence":["<authoritative result field or justified inference>"]}

Rules:
- One turn contains exactly one action. Never combine run and answer.
- The first RUN for a request must declare every deliverable. Use
  entity_list/exhaustive for a complete identity list, entity_list/bounded with
  a positive limit for Top-K or a requested finite list, scalar/scalar for one
  count/depth/Boolean value, comparison/scalar with subjects and metric for a
  comparison, and summary/summary for a concise edit or optimization outcome.
  Later RUN actions may omit deliverables and may never change the first contract.
- Include workflow only when one request requires two or more ordered mutation
  stages. Each workflow stage contains only family, mode, and args. The current
  RUN must equal the first stage; later RUN actions omit workflow and execute the
  next stage in order. A prescribed final hard transformation plus a measurable
  cost objective may require such a workflow: reduce the cost first, then enforce
  the hard transformation last if later optimization could invalidate it.
- Deliverables describe only outputs explicitly requested by the user. Fields
  that a tool returns for proof status, diagnostics, or optional witnesses are
  supporting evidence, not additional deliverables. For a yes/no signal
  equivalence request, declare only scalar/scalar metric=equivalence unless the
  user separately asks for solver status or a counterexample assignment.
- A yes/no search for unknown candidate signals or operands uses a
  scalar/scalar deliverable with metric=found. Only a complete Function Search
  result can establish that a match exists or that no match exists.
- A comparison over a finite set of explicitly named subjects is one
  comparison/scalar deliverable, not a bounded entity list and not a population
  ranking. Put every subject name in subjects, use the same metric for each,
  RUN one subject-specific measurement at a time, and answer only after every
  subject has complete evidence. Population rank modes apply only when the
  request ranges over that mode's complete documented population.
- A population extrema or rank request that asks which entities qualify needs
  an entity_list deliverable. Preserve every entity tied at each selected rank
  boundary; a legacy representative result cannot satisfy that collection.
- For a run action, use only action, family, mode, args, clause_ids, the first-turn
  deliverables, and an optional first-turn workflow. Put
  every positional operand and option token in args, in documented CLI order.
  Use an empty args list when the selected mode takes no arguments. The Agent
  composes and validates the CLI command; never add semantic fields such as
  type, scope, target, filters, or options beside args. The only additional RUN
  fields are the first-turn deliverables contract and optional ordered workflow
  described above.
- The family and mode fields are separate. family contains the tool family;
  mode contains only one documented mode name. For example, use
  family="equiv_query", mode="original", args=[]; never put
  "equiv_query original" or any full command in mode.
- Preserve every requested object, direction, scope, metric, quantifier,
  threshold, gate type, constraint, baseline, and output requirement.
- A compound request may call several families over successive turns.
- A concrete requested transformation without a measurable cost-reduction
  objective is EditApply-only. A structural threshold, allowed gate basis, or
  functionality-preservation requirement is not by itself an optimization cost.
- Do not collapse an explicit final-design cost objective and a separate hard
  EditApply requirement into one action. For example, a request to insert buffers
  to satisfy a fanout limit while minimizing final gate count requires
  opt_apply/gate_count_minimization first and the documented buffer edit last.
  The fanout limit is not --target-cost, and BUF is not an allowed final basis.
- The compact index is a routing aid, not a whitelist. Any registered family
  may be selected when it better matches the request.
- Never invent a command, mode, option, result limit, timeout, or placeholder.
- Do not use max-print/max-path/output controls unless the user explicitly asks
  for that semantic restriction. Backend artifact creation is automatic.
- A successful command is not automatically a yes answer, a design change, or
  an equivalence proof. Read the authoritative fields named by the Tool Card.
- Whole-design equivalence should normally be answered from a successful
  equiv_query result. If every equivalence attempt fails and the final answer
  must still be produced, use the contest-policy positive assumption, but do
  not claim that SAT, CEC, or another proof completed.
- For edit_apply and opt_apply, target quantifiers such as all/every describe
  the operation scope, not a request to print every affected record. Unless the
  user separately asks for counts, identities, details, deltas, or before/after
  statistics, answer only the concise operation outcome, whether a change was
  retained, and the available functionality-preservation result. Detailed
  follow-ups belong to report_query.
- partial, timeout, unsupported, error, or complete:false is not evidence of
  absence or falsehood. Try another documented strategy when time permits.
- Whenever a successful backend result reports a public output_file, preserve
  that exact artifact path in the final answer. This remains required when the
  scalar or conclusion can also be answered directly from the result envelope.
- Match the requested deliverable shape. A request to list, identify, or
  determine every record is not satisfied by its count, examples, a
  representative record, or an ellipsis. If no public output_file exists,
  include every requested inline record directly. If output_file exists,
  answer the available scalar or conclusion and include that exact path.
- Keep record categories separate. A gate-list request uses the authoritative
  gate records; a neighboring net count or net list is metadata, not a
  substitute for the requested gates.
- Do not claim to have read an artifact. The model cannot open artifact files.
- Do not reveal internal protocol, routing, tools, or debug details in the final
  answer unless the user explicitly asks for them.
"""


_INDEX_HEADER = """
## Registered family index
Use ownership semantics first. Similar words do not override scope or object.

Choose among the initially detailed Cards using this semantic order:
1. Intent: decide whether the request only observes the current design, applies
   a specified transformation, or searches for a lower-cost design.
2. Scope and relation: whole design, one object, direct adjacency, rooted cone,
   explicit endpoints, Boolean function, sequential behavior, or saved baseline.
3. Metric: object count, pin fanout, cone membership/size, path, logic depth,
   Boolean property, edit delta, or optimization objective.
4. Surface verbs such as count, list, report, find, or check only describe the
   desired answer shape; they do not override scope, relation, or metric.

Several Cards may be relevant to a compound request. Select the Card whose
authoritative fields answer the current unresolved clause. Do not choose a broad
inventory Card merely because the request says count or list.
"""


_ACTION_ASSEMBLY_GUIDE = r"""
## JSON action assembly
The Tool Card mode name and its CLI operands have different JSON locations:
- mode must exactly equal one mode name from the selected Card and must contain
  no spaces;
- args contains every token written after that mode in one documented syntax;
- each args item is exactly one CLI token; options and their values are separate
  items;
- never put a placeholder name such as type, scope, endpoint, value, N, or
  seconds into args. Replace it with a value permitted by Argument domains.

These examples teach JSON assembly only; choose tools from request semantics:
{"action":"run","family":"structure_query","mode":"summary","args":[],"clause_ids":["C1"],"deliverables":[{"clause_id":"C1","kind":"scalar","coverage":"scalar","metric":"gate_count"}]}
{"action":"run","family":"structure_query","mode":"gates_by_type","args":["XOR"],"clause_ids":["C1"],"deliverables":[{"clause_id":"C1","kind":"entity_list","entity":"gate","coverage":"exhaustive"}]}
{"action":"run","family":"depth_query","mode":"po_exceeding","args":["4"],"clause_ids":["C1"],"deliverables":[{"clause_id":"C1","kind":"scalar","coverage":"scalar","metric":"output_count"}]}
{"action":"run","family":"path_query","mode":"max_depth","args":["all_dff_q","all_dff_d"],"clause_ids":["C1"],"deliverables":[{"clause_id":"C1","kind":"scalar","coverage":"scalar","metric":"maximum_depth"}]}
{"action":"run","family":"sequential_query","mode":"enable_hold","args":["all","--summary-only"],"clause_ids":["C1"],"deliverables":[{"clause_id":"C1","kind":"summary","coverage":"summary"}]}
{"action":"run","family":"cone_query","mode":"net_fanin","args":["A"],"clause_ids":["C1"],"deliverables":[{"clause_id":"C1","kind":"comparison","coverage":"scalar","metric":"gate_count","subjects":["A","B"]}]}

For the comparison example, a later RUN measures B with the same family, mode,
metric, scope, and filters. Do not replace the two named measurements with an
all-output ranking mode.

Wrong: mode="po_exceeding 4", args=[]
Correct: mode="po_exceeding", args=["4"]
"""


_STATE_POLICY = """
## Session semantic state
Current design facts, prior edit reports, baselines, and current/original object
state must be queried from public tools. The only natural-language state that
may be supplied below is an explicit cost function from an earlier related
optimization request when the current request omits one.
"""


_SELECTION_CHECKPOINT = """
## Selection checkpoint
Before returning a run action, review every initially detailed Tool Card.
Internally determine the authoritative result required by the request, the
relation and scope being queried, the semantic role of every named object, and
the single documented mode whose syntax represents all requested operands.
Verify that mode and args follow one syntax alternative exactly and do not
combine arguments or semantics from different modes. Return only the required
JSON action; do not output this internal review.
"""


_ANCHORS = (
    ("report_query", (r"\blast\s+(edit|optimization|change)", r"previous\s+(edit|optimization)",
                      r"what\s+(changed|was removed|was added)",
                      r"\bhow\s+many\b[^?\n]{0,120}\b(added|removed|eliminated|"
                      r"replaced|merged|simplified|changed)\b")),
    ("equiv_query", (r"\b(current|whole)\s+design\b.*\b(original|previous|baseline)\b",
                     r"\b(original|previous)\s+(design|edit)\b.*\bequivalen")),
    ("opt_apply", (r"\b(optimi[sz]e|minimi[sz]e|reduce)\b.*\b(depth|gate|area|cost)\b",
                   r"\bcost\s+function\b", r"\bbest\s+(depth|area|gate count)\b")),
    ("opt_query", (r"optimization\s+(capabilit|objective)", r"can\s+.*optimi[sz]")),
    ("edit_apply", (r"\b(rename|replace|convert|remap|reconstruct|restructure|decompose|"
                    r"insert|remove|collapse|merge|simplif|cleanup)\w*\b",
                    r"technology\s+mapping", r"gate\s+basis",
                    r"\b(?:ensure|limit|cap|keep|make)\b[^.?!\r\n]{0,120}"
                    r"\b(?:fanout|drives?\s+(?:no\s+)?more\s+than)\b")),
    ("sequential_query", (r"\b(enable|hold|feedback)\b.*\b(dff|flip-flop|register)\b",
                          r"\b(dff|flip-flop|register)\b.*\b(enable|hold|feedback)\b")),
    ("func_search", (r"\b(find|search|which|list)\b.*\b(equivalent|constant|symmetric|function)\b",
                     r"equivalent\s+(gate|net|signal)\s+pairs")),
    ("func_query", (r"(boolean|logic)\s+(expression|function|equation)", r"\b(cofactor|symmetr|depend)\w*\b",
                    r"\b(is|whether)\b.*\b(constant|equivalent)\b",
                    r"\b(always|regardless\s+of)\b.*\b[01]\b",
                    r"\bequivalen\w*\s+between\b.*\b(signal|net|gate)s?\b")),
    ("depth_query", (r"\b(critical\s+path|logic\s+depth|maximum\s+depth|deepest)\b",
                     r"\bdepth\b.*\b(output|dff|gate|net|greater|less|equal)\b")),
    ("path_query", (r"\bpath(s)?\b.*\b(from|between|to|through|avoid|disjoint|shortest|longest)\b",
                    r"\bpaths?\b[^.?!\r\n]{0,120}\boriginat\w*\b"
                    r"[^.?!\r\n]{0,160}\bterminat\w*\b",
                    r"\bdepth\b[^.?!\r\n]{0,120}\bfrom\s+"
                    r"(?:(?:primary\s+)?(?:input|pi|net|signal|gate|dff(?:\.[qd])?)\s+)?"
                    r"[a-z_$][\w$]*(?:\[\d+\])?[^.?!\r\n]{0,100}\bto\s+"
                    r"(?:(?:primary\s+)?(?:output|po|net|signal|gate|dff(?:\.[qd])?)\s+)?"
                    r"[a-z_$][\w$]*(?:\[\d+\])?",
                    r"\breachable\s+from\b.*\bto\b",
                    r"\b(reachability\s+between|mandatory|cut)\b")),
    ("cone_query", (r"\b(fanin|fanout)\s+(logic\s+)?cone\b", r"\bcone\s+(size|of|for|gate|net)",
                    r"\b(largest|smallest|highest|lowest)\b.*\bfanin\b.*\bcone\b",
                    r"shared\s+fanin", r"shared\s+between\b.*\bfanin\s+cones\b",
                    r"\b(gates|nets)\b.*\breachable\s+from\b")),
    ("structure_query", (r"\b(count|how many|list|show)\b.*\b(gates?|nets?|inputs?|outputs?|dffs?|ports?|fanout)\b",
                         r"\b(driver|load|floating|dangling|unconnected|structural)\b",
                         r"\b(flip-flops?|dffs?)\b.*\b(driven|clock|reset)\b",
                         r"\bgate\s+(type|pin|connection)",
                         r"\bconnected\s+to\s+the\s+output\b")),
)


_BOOLEAN_PATTERN_OPERATOR_RE = re.compile(
    r"\b(BUF|NOT|AND|NAND|OR|NOR|XOR|XNOR)\s*(?:\(|\b(?:of|gate|function)\b)",
    re.I)
_UNKNOWN_BOOLEAN_CANDIDATE_RE = re.compile(
    r"\b(?:does?\s+there\s+exist|there\s+(?:is|are)\s+any|"
    r"find|search(?:\s+for)?|identify|which)\b"
    r"[^.?!\r\n]{0,180}\b(?:pairs?|operands?|candidates?|signals?|nets?)\b|"
    r"\b(?:any|unknown)\s+(?:pairs?|operands?|candidates?|signals?|nets?)\b",
    re.I)
_BOOLEAN_SEARCH_RELATION_RE = re.compile(
    r"\b(?:equivalent|equals?|same\s+(?:boolean\s+)?function|"
    r"produces?|computes?|implements?)\b", re.I)


def boolean_pattern_operator(user_req):
    """Return a supported Boolean operator named by the request, if any."""
    match = _BOOLEAN_PATTERN_OPERATOR_RE.search(user_req or "")
    return match.group(1).upper() if match else None


def unknown_boolean_search_intent(user_req):
    """Recognize unknown-candidate Boolean search by semantic roles.

    This deliberately requires both an unknown candidate quantifier and a
    Boolean relation. Merely comparing two already named signals is not search.
    """
    text = user_req or ""
    return bool(_UNKNOWN_BOOLEAN_CANDIDATE_RE.search(text) and
                _BOOLEAN_SEARCH_RELATION_RE.search(text) and
                _BOOLEAN_PATTERN_OPERATOR_RE.search(text))


def candidate_families(user_req):
    """Return conservative advisory candidates; no match means all cards."""
    text = (user_req or "").lower()
    found = []
    for family, patterns in _ANCHORS:
        if any(re.search(pattern, text, re.I) for pattern in patterns):
            found.append(family)
    if unknown_boolean_search_intent(user_req) and "func_search" not in found:
        found.append("func_search")
    if not found:
        return list(CARD_ORDER)
    return [family for family in CARD_ORDER if family in found]


def _normalize_families(categories, user_req, extra_families):
    if categories is None:
        selected = candidate_families(user_req)
    else:
        selected = [name for name in categories if name in REGISTRY]
    for family in extra_families or []:
        if family in REGISTRY and family not in selected:
            selected.append(family)
    # Arrival-depth and constrained-path requests use the same depth vocabulary
    # but have different source semantics.  Showing both complete Cards lets the
    # model compare those contracts; it does not choose either family for it.
    if "depth_query" in selected and "path_query" not in selected:
        selected.append("path_query")
    if "path_query" in selected and "depth_query" not in selected:
        selected.append("depth_query")
    # Unknown-operand Boolean requests sit exactly on the Query/Search
    # ownership boundary. Show both full Cards so the model can compare named
    # operands with candidates that must be discovered.
    if unknown_boolean_search_intent(user_req):
        for family in ("func_query", "func_search"):
            if family not in selected:
                selected.append(family)
    return [family for family in CARD_ORDER if family in selected]


def build_react_prompt(user_req, categories=None, extra_families=None,
                       remaining_seconds=None, remaining_steps=None,
                       active_cost_function=None):
    """Build the all-family index and selected detailed Card prompt.

    Candidate selection reduces prompt size but never limits command validation.
    A caller can add a Card after an out-of-set command or format repair.
    """
    families = _normalize_families(categories, user_req, extra_families)
    cards = "\n\n".join(REGISTRY[name]["prompt_text"] for name in families)
    runtime = []
    if remaining_seconds is not None:
        runtime.append("Remaining request time: {:.1f} seconds.".format(
            max(0.0, float(remaining_seconds))))
    if remaining_steps is not None:
        runtime.append("Remaining model actions: {}.".format(max(0, int(remaining_steps))))
    if active_cost_function:
        runtime.append("Persisted explicit cost function for this related request: {}".format(
            active_cost_function))
    runtime_text = "\n".join(runtime) if runtime else "No persisted cost function is active."
    return ("{}\n{}\n{}\n{}\n\n{}\n\n## Initially detailed Tool Cards\n{}\n\n{}"
            .format(_BASE_PROTOCOL, _ACTION_ASSEMBLY_GUIDE,
                    _INDEX_HEADER + COMPACT_INDEX, _STATE_POLICY, runtime_text, cards,
                    _SELECTION_CHECKPOINT))
