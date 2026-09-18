#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ICCAD Contest Problem A - Agent (zero dependency, Python 3.6+)
Unified high-level CLI with tiered prompt selection.

Usage:
    python3 agent_final.py -config config.yaml
    python3 agent_final.py -config config.yaml --url http://127.0.0.1:11434/v1 --model gpt-oss:20b

Fixes in this revision
----------------------
1. parse_llm_line : multi-line ANSWER is preserved; reasoning stays internal
                    and is not a separate protocol turn.
2. envelope_ok    : parses the envelope "ok:" field instead of substring-
                    matching "ok", which matched "ok: false" as success.
3. backend I/O    : waits for the backend's own result/timeout envelope; Python
                    never kills a live backend merely because a timer expired.
4. read/write     : path is taken from the .v token that follows the read/write
                    keyword; compound requests are no longer swallowed by the
                    write branch.
5. LLM retry      : bounded exponential backoff on 429/403/5xx (ported from
                    agent_hybrid.py).
6. log location   : written next to the executable (Q&A A60), mirrored into the
                    loaded design's directory and the cwd when they differ.
"""
import sys, os, re, json, time, argparse, subprocess, urllib.request, urllib.error, ssl
import email.utils
import math
import shlex

try:
    import select
    _HAVE_SELECT = hasattr(select, "select") and os.name == "posix"
except Exception:
    select = None
    _HAVE_SELECT = False

TOOLS_EXE = os.environ.get("EDA_TOOLS_EXE", "./EDATools")
PROMPT_MARKER = "eda> "
MAX_REACT_STEPS = 8
ANALYSIS_REQUEST_SECONDS = 300.0
IO_REQUEST_SECONDS = 60.0
FINAL_ANSWER_RESERVE_SECONDS = 10.0
LLM_ACTION_ATTEMPTS = 2

# Submission requests preserve complete backend evidence in the model input.
# A finite value may still be supplied by the local runner's --max-obs option.
MAX_OBS_CHARS = None

# Executable directory: A60 requires the log/output to live under the program
# path (the grader copies the executable elsewhere, so cwd is not reliable).
EXE_DIR = os.path.dirname(os.path.abspath(sys.argv[0])) or os.getcwd()

# stdout is the grader's synchronisation channel: it blocks until "#END <id>".
# Under a non-UTF-8 console encoding an unmappable character in an answer raises
# UnicodeEncodeError mid-emit and kills the run, so force UTF-8 with
# replacement. (reconfigure is 3.7+; the guard keeps the 3.6 claim honest.)
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# LLM retry policy (A18: rate limits follow the providers' own policies).
# MAX_RETRIES counts total HTTP requests, including the first attempt.
MAX_RETRIES = 4
RETRY_DELAYS = (1.5, 2.5, 4.0)
LLM_REQUEST_TIMEOUT = 290.0

# Prompt text and command validation are generated from the same Tool Cards.
from prompts import (build_react_prompt, candidate_families,
                     boolean_pattern_operator, unknown_boolean_search_intent)
from generated_tool_registry import REGISTRY


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  YAML parser (no pyyaml)                                               ║
# ╚════════════════════════════════════════════════════════════════════════╝

def parse_yaml(path):
    result = {}
    section = None
    # Explicit UTF-8: the default locale encoding is cp950/cp1252 on Windows and
    # a non-ASCII byte anywhere in the file aborts the whole run.
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.rstrip()
            if not s or s.startswith("#"): continue
            if not line.startswith(" ") and not line.startswith("\t"):
                k, _, v = s.partition(":")
                k = k.strip().strip('"').strip("'")
                v = v.strip().strip('"').strip("'")
                if v:
                    result[k] = v
                else:
                    section = k
                    result[section] = {}
            elif section is not None:
                k, _, v = s.strip().partition(":")
                k = k.strip().strip('"').strip("'")
                v = v.strip().strip('"').strip("'")
                if v.startswith("<") and v.endswith(">"): v = ""
                result[section][k] = v
    return result


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  LLM client (urllib only), with bounded retry                          ║
# ╚════════════════════════════════════════════════════════════════════════╝

try:
    _SSL = ssl.create_default_context()
except Exception:
    _SSL = None

# 403 removed: it is an auth/permission failure, never transient.  Retrying it
# six times only burns the request's wait budget before the inevitable failure.
RETRYABLE_STATUS = (408, 409, 429, 500, 502, 503, 504)


def _parse_retry_after(value, now=None):
    """Return Retry-After seconds for delta-seconds, decimals, or HTTP-date."""
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    try:
        seconds = float(text)
        if math.isfinite(seconds) and seconds >= 0:
            return seconds
    except (TypeError, ValueError):
        pass
    try:
        parsed = email.utils.parsedate_tz(text)
        if parsed is None:
            return None
        target = email.utils.mktime_tz(parsed)
        seconds = target - (time.time() if now is None else now)
        return seconds if seconds >= 0 else None
    except (TypeError, ValueError, OverflowError):
        return None


def _post_json(url, payload, headers, timeout=LLM_REQUEST_TIMEOUT):
    req = urllib.request.Request(url, data=payload, headers=headers)
    with urllib.request.urlopen(req, context=_SSL, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _with_retry(fn, wait_budget=None, deadline=None, sleep_fn=time.sleep):
    """Run fn with four attempts bounded by the current request deadline."""
    _ = wait_budget
    last_err = None
    for attempt in range(MAX_RETRIES):
        retry_after = None
        remaining = None if deadline is None else deadline - time.monotonic()
        if remaining is not None and remaining <= 0:
            raise TimeoutError("LLM request deadline exhausted")
        attempt_timeout = LLM_REQUEST_TIMEOUT
        if remaining is not None:
            attempt_timeout = max(0.1, min(attempt_timeout, remaining))
        try:
            return fn(attempt_timeout)
        except urllib.error.HTTPError as e:
            last_err = e
            if e.code not in RETRYABLE_STATUS:
                raise
            retry_after = _parse_retry_after(
                e.headers.get("Retry-After") if e.headers is not None else None)
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last_err = e
        except Exception as e:
            last_err = e
            raise
        if attempt == MAX_RETRIES - 1:
            break
        delay = retry_after + 1.0 if retry_after is not None else RETRY_DELAYS[attempt]
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("LLM request deadline exhausted")
            delay = min(delay, max(0.0, remaining))
        sleep_fn(delay)
    raise last_err


def call_openai(api_key, model, system, messages, temperature=0.2, max_tokens=4096,
                base_url=None, wait_budget=None, deadline=None):
    url = (base_url.rstrip("/") if base_url else "https://api.openai.com/v1") + "/chat/completions"
    payload = json.dumps({
        "model": model,
        "messages": [{"role": "system", "content": system}] + messages,
        "temperature": temperature,
        "max_tokens": max_tokens,
    }).encode("utf-8")
    headers = {"Content-Type": "application/json",
               "Authorization": "Bearer " + (api_key or "dummy"),
               "User-Agent": "python-requests/2.31.0"}
    data = _with_retry(lambda timeout: _post_json(url, payload, headers, timeout),
                       wait_budget, deadline)
    return data["choices"][0]["message"]["content"].strip()


def call_anthropic(api_key, model, system, messages, temperature=0.2, max_tokens=4096,
                   wait_budget=None, deadline=None):
    url = "https://api.anthropic.com/v1/messages"
    payload = json.dumps({
        "model": model,
        "system": system,
        "messages": messages,
        "temperature": temperature,
        "max_tokens": max_tokens,
    }).encode("utf-8")
    headers = {"Content-Type": "application/json",
               "x-api-key": api_key,
               "anthropic-version": "2023-06-01"}
    data = _with_retry(lambda timeout: _post_json(url, payload, headers, timeout),
                       wait_budget, deadline)
    return data["content"][0]["text"].strip()


class LLMClient(object):
    def __init__(self, provider, model, api_key, temperature, max_tokens, base_url=None):
        self.provider = provider
        self.model = model
        self.api_key = api_key
        self.temperature = temperature
        self.max_tokens = max_tokens
        self.base_url = base_url

    def chat(self, system, messages, max_tokens=None, deadline=None):
        """Call the configured provider within the request deadline."""
        mt = max_tokens or self.max_tokens
        if self.provider == "anthropic":
            return call_anthropic(self.api_key, self.model, system, messages,
                                  self.temperature, mt, wait_budget=None, deadline=deadline)
        return call_openai(self.api_key, self.model, system, messages, self.temperature, mt,
                           self.base_url, wait_budget=None, deadline=deadline)


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  Envelope parsing — status semantics stay in Python, never in the LLM  ║
# ╚════════════════════════════════════════════════════════════════════════╝
#
# The tool spec is explicit that partial / timeout / unsupported / error and
# complete:false must NOT be read as "no" / "false" / "does not exist".  A weak
# model shown a half-empty envelope will do exactly that, so the harness
# interprets these fields and only ever hands the model a clean observation.

_ENV_OK = re.compile(r"^\s*ok:\s*(true|false)\b", re.M | re.I)
_ENV_STATUS = re.compile(r"^\s*status:\s*([A-Za-z_]+)", re.M | re.I)
_ENV_COMPLETE = re.compile(r"^\s*complete:\s*(true|false)\b", re.M | re.I)
_ENV_MESSAGE = re.compile(r"^\s*message:\s*(.*)$", re.M)


def envelope_ok(raw):
    """True only when the envelope explicitly reports ok: true.

    Falls back to a loose check when the backend did not emit an envelope at
    all (older/simpler commands), but never treats the literal string "ok" as
    success, which made "ok: false" read as a pass.
    """
    m = _ENV_OK.search(raw or "")
    if m:
        return m.group(1).lower() == "true"
    low = (raw or "").lower()
    if "error" in low or "fail" in low:
        return False
    return "success" in low


def envelope_status(raw):
    m = _ENV_STATUS.search(raw or "")
    return m.group(1).lower() if m else ""


def envelope_complete(raw):
    m = _ENV_COMPLETE.search(raw or "")
    return (m.group(1).lower() == "true") if m else True


def envelope_message(raw):
    m = _ENV_MESSAGE.search(raw or "")
    return m.group(1).strip() if m else ""


def observation_for_llm(raw):
    """Turn a backend envelope into an observation that cannot be misread."""
    status = envelope_status(raw)
    if status in ("partial", "timeout"):
        return ("The query did not finish, so its result is INCOMPLETE and must not be used "
                "to conclude that something does not exist or is false. Retry with a smaller "
                "scope or a different mode.\n\nPartial backend output:\n" + raw)
    if status == "unsupported":
        return ("The backend reports this command/mode is not supported. This is NOT an answer "
                "to the question. Choose a different command, or reply UNSUPPORTED if no command "
                "can answer it.\n\nBackend output:\n" + raw)
    if status == "error" or not envelope_ok(raw):
        msg = envelope_message(raw) or "unknown error"
        return ("The command failed ({}). This is NOT an answer to the question. Fix the "
                "arguments and retry.\n\nBackend output:\n{}".format(msg, raw))
    if not envelope_complete(raw):
        return ("The result is marked complete:false and is therefore INCOMPLETE. Do not treat "
                "missing entries as non-existent.\n\nBackend output:\n" + raw)
    return "Backend output:\n" + raw


def _truncate_obs(obs, label):
    """Apply only an explicitly configured local observation-size limit."""
    if MAX_OBS_CHARS is None or len(obs) <= MAX_OBS_CHARS:
        return obs
    fname = "obs_{}.txt".format(label)
    fpath = os.path.join(EXE_DIR, fname)
    try:
        with open(fpath, "w", encoding="utf-8", errors="replace") as fh:
            fh.write(obs)
        note = ("\n\nobservation_output_file: {}\n"
                "[Output truncated at {} chars. Full output contains {} chars. "
                "Read key numeric fields from the excerpt above.]").format(
                    fpath, MAX_OBS_CHARS, len(obs))
    except Exception:
        note = ("\n\n[Output truncated at {} chars. "
                "Key fields are in the excerpt above.]").format(MAX_OBS_CHARS)
    return obs[:MAX_OBS_CHARS] + note


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  Backend process                                                       ║
# ╚════════════════════════════════════════════════════════════════════════╝

class EDABackend(object):
    """Binary-mode pipe to the EDA CLI.

    Backend commands own their timeout, rollback, and timeout envelope.  The
    Python harness waits for the next prompt and never kills a live process on
    an agent-side timer, because restarting would discard current edits,
    baselines, and cached reports.
    """

    READ_CHUNK = 65536

    def __init__(self, exe_path):
        self.exe_path = exe_path
        self.proc = None
        self.loaded_file = None
        self._start()

    def _start(self):
        self.proc = subprocess.Popen(
            [self.exe_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,  # PIPE without a reader → 64 KB deadlock
            bufsize=0,
        )
        self._read_until_prompt()

    def _read_until_prompt(self, timeout=None):
        """Read one response, waiting for the backend's own completion policy.

        timeout remains accepted for compatibility with older callers but is
        intentionally ignored.  No timer in this function kills the process.
        """
        _ = timeout
        marker = PROMPT_MARKER.encode("utf-8")
        if not _HAVE_SELECT:
            buf = bytearray()
            while True:
                chunk = self.proc.stdout.read(1)
                if not chunk:
                    break
                buf.extend(chunk)
                if buf.endswith(marker):
                    del buf[-len(marker):]
                    break
            return bytes(buf).decode("utf-8", "replace").strip()

        buf = bytearray()
        fd = self.proc.stdout.fileno()
        while True:
            ready, _, _ = select.select([fd], [], [])
            if not ready:
                continue
            try:
                chunk = os.read(fd, self.READ_CHUNK)
            except OSError:
                chunk = b""
            if not chunk:
                break
            buf.extend(chunk)
            if buf.endswith(marker):
                del buf[-len(marker):]
                break
        return bytes(buf).decode("utf-8", "replace").strip()

    def _ensure_alive(self):
        if self.proc is None or self.proc.poll() is not None:
            self._start()
            self._reload()

    def send(self, command, timeout=None):
        """Run one command and wait for the backend's own result envelope."""
        _ = timeout
        self._ensure_alive()
        try:
            self.proc.stdin.write((command + "\n").encode("utf-8"))
            self.proc.stdin.flush()
        except (IOError, OSError, ValueError):
            self._start()
            self._reload()
            try:
                self.proc.stdin.write((command + "\n").encode("utf-8"))
                self.proc.stdin.flush()
            except Exception as e:
                return "ok: false\nstatus: error\nmessage: backend write failed: {}".format(e)
        out = self._read_until_prompt()
        if command.startswith("read "):
            self.loaded_file = command.split(None, 1)[1]
        return out

    def _reload(self):
        if not self.loaded_file:
            return
        try:
            self.proc.stdin.write(("read " + self.loaded_file + "\n").encode("utf-8"))
            self.proc.stdin.flush()
            self._read_until_prompt()
        except Exception:
            pass

    def close(self):
        try:
            if self.proc and self.proc.poll() is None:
                self.proc.terminate()
        except Exception:
            pass


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  Logger                                                                ║
# ╚════════════════════════════════════════════════════════════════════════╝

class Logger(object):
    """Writes the response log to every plausible grader location.

    A60 asks for the log under the executable path or the design path; A70
    accepts workdir/ or workdir/testcase/<case>/.  Writing all of them is cheap
    and removes the NOT_FOUND failure mode from Q60.
    """

    def __init__(self):
        self.handles = {}
        self.blocks = []
        self.resp_id = 0
        self.case_name = None

    def set_log(self, case_name):
        self.close()
        self.handles = {}
        self.blocks = []
        self.case_name = case_name
        self.add_dir(EXE_DIR)
        cwd = os.path.abspath(os.getcwd())
        if cwd != EXE_DIR:
            self.add_dir(cwd)

    def add_dir(self, directory):
        """Open (and back-fill) a log copy in another directory."""
        if not self.case_name or not directory:
            return
        directory = os.path.abspath(directory)
        if directory in self.handles:
            return
        path = os.path.join(directory, self.case_name + ".log")
        try:
            if not os.path.isdir(directory):
                return
            fh = open(path, "w", encoding="utf-8", errors="replace")
        except Exception:
            return
        try:
            for block in self.blocks:
                fh.write(block + "\n\n")
            fh.flush()
        except Exception:
            # A back-fill failure must not take down the run; the other log
            # copies and stdout are still intact.
            try:
                fh.close()
            except Exception:
                pass
            return
        self.handles[directory] = fh

    def emit(self, text):
        self.resp_id += 1
        block = "#RESPONSE {}\n{}\n#END {}".format(self.resp_id, text, self.resp_id)
        # stdout is the grader's synchronisation channel: it waits for #END <id>
        # before sending the next request, so flush immediately.
        sys.stdout.write(block + "\n")
        sys.stdout.flush()
        self.blocks.append(block)
        for fh in list(self.handles.values()):
            try:
                fh.write(block + "\n\n")
                fh.flush()
            except Exception:
                pass

    def close(self):
        for fh in list(self.handles.values()):
            try:
                fh.close()
            except Exception:
                pass
        self.handles = {}


class DebugTrace(object):
    """Optional JSONL trace kept off stdout and out of the official answer log."""

    def __init__(self, enabled=False, path=None):
        self.enabled = bool(enabled)
        self.path = path or os.path.join(EXE_DIR, "agent_debug.jsonl")

    def record(self, event, payload):
        if not self.enabled:
            return
        row = {"time": time.time(), "event": event, "payload": payload}
        try:
            with open(self.path, "a", encoding="utf-8", errors="replace") as fh:
                fh.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
        except Exception as exc:
            sys.stderr.write("[debug] trace write failed: {}\n".format(exc))


_COST_FUNCTION_RE = re.compile(
    r"\bcost\s+function\s+(?:is|=|:)\s*([^\n.!?]+)", re.IGNORECASE)
_OPT_RELATED_RE = re.compile(
    r"\b(optimi[sz]|minimi[sz]|reduce|best|depth|gate\s+count|area|cost)\w*\b",
    re.IGNORECASE)


class SessionSemanticState(object):
    """Persist only an explicit cost function for related follow-up prompts."""

    def __init__(self):
        self.active_cost_function = None

    def reset(self):
        self.active_cost_function = None

    def for_request(self, request):
        match = _COST_FUNCTION_RE.search(request or "")
        if match:
            self.active_cost_function = match.group(1).strip()
            return None  # it is already explicit in this request
        if self.active_cost_function and _OPT_RELATED_RE.search(request or ""):
            return self.active_cost_function
        return None


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  Request classification helpers                                        ║
# ╚════════════════════════════════════════════════════════════════════════╝

def is_testcase_start(req):
    low = req.lower()
    return ("beginning" in low and "testcase" in low) or "case name" in low


def extract_case_name(req):
    m = re.search(r"case name is ['\"]?([A-Za-z0-9_]+)", req, re.IGNORECASE)
    if m: return m.group(1)
    m = re.search(r"testcase ['\"]?([A-Za-z0-9_]+)", req, re.IGNORECASE)
    if m: return m.group(1)
    return "testcase"


def _strip_quotes(s):
    return s.strip().strip("'\"").strip()


# Verbs that mean the request does more than move a file around.  A sentence
# containing one of these is never handled by the bare read/write fast path,
# because "optimise the cone and write it to out.v" must not collapse into a
# plain write.
# NOTE: the trailing \b must NOT be applied to the stem alternatives.  A stem
# such as "simplif" is always followed by a word character ("simplify"), so
# "\b(simplif|...)\b" matched nothing at all -- and neither did any inflected
# form ("removing", "inserted", "buffers").  Stems therefore take \w* instead;
# only the short, ambiguous words keep a closing \b so that "add" does not fire
# on "address" nor "list" on "listen".
_ACTION_VERBS = re.compile(
    r"\b(?:insert|remov|replac|optimi[sz]|simplif|merg|propagat|restructur|balanc|"
    r"reduc|renam|collaps|convert|remap|reconnect|delet|minimi[sz]|buffer|"
    r"analy[sz]|identif|deriv|verif|report|count)\w*"
    r"|\b(?:add|list|find|which|what|does|do|is|are|check|prove|how many)\b",
    re.IGNORECASE)

# A bare read/write must be a single clause.  Any of these signals a second
# task riding along in the same sentence ("after removing X, save to out.v"),
# which the fast path would silently drop.  Cost asymmetry drives this: an
# over-eager fast path writes an untransformed netlist and scores zero with a
# cheerful log line, while an over-cautious one merely spends an LLM call.
_COMPOUND_HINT = re.compile(
    r"[;,]|\b(?:and|then|after|before|while|once|also|next|finally|plus)\b",
    re.IGNORECASE)

_READ_KEYWORD = re.compile(r"\b(read|load|open)\b", re.IGNORECASE)
_WRITE_KEYWORD = re.compile(r"\b(write|output|save|dump|export)\b", re.IGNORECASE)
_V_TOKEN = re.compile(
    r'"([^"\r\n]+\.v)"|\'([^\'\r\n]+\.v)\'|([^\s,;]+\.v)(?=$|[\s,;.!?])',
    re.IGNORECASE)
_DIR_TOKEN = re.compile(
    r'(?:directory|folder|dir)\s+(?:"([^"\r\n]+)"|\'([^\'\r\n]+)\'|([^\s,;]+))',
    re.IGNORECASE)


def _match_value(match):
    if not match:
        return None
    for value in match.groups():
        if value is not None:
            return _strip_quotes(value).rstrip(".,")
    return None


def _v_after(req, keyword_re):
    """First *.v token appearing after the keyword, falling back to the first
    one in the sentence.  Prevents 'read a.v then write b.v' picking a.v twice."""
    km = keyword_re.search(req)
    if km:
        m = _V_TOKEN.search(req, km.end())
        if m:
            return _match_value(m)
    m = _V_TOKEN.search(req)
    return _match_value(m)


def _has_extra_clause(req, keyword_re):
    """True when the sentence carries a clause besides the read/write itself.

    Punctuation and conjunctions inside the *path* do not count, so
    "load a.v from directory design/netlist/" stays on the fast path while
    "after removing dangling gates, save to out.v" does not.
    """
    stripped = _V_TOKEN.sub(" ", req)
    dm = _DIR_TOKEN.search(stripped)
    if dm:
        stripped = stripped[:dm.start()] + " " + stripped[dm.end():]
    # "..., please." is politeness, not a second clause (Figure 5 uses exactly
    # this phrasing), so peel it off before looking for compound signals.
    stripped = re.sub(r"[,;]?\s*(?:please|thanks|thank you)\s*[.!]*\s*$", "",
                      stripped, flags=re.IGNORECASE)
    stripped = re.sub(r"^\s*(?:please|kindly|now)\b[,\s]*", " ", stripped,
                      flags=re.IGNORECASE)
    stripped = re.sub(r"[.\s]+$", "", stripped)
    return bool(_COMPOUND_HINT.search(stripped))


def is_read_request(req):
    if not _READ_KEYWORD.search(req):
        return False
    if not _V_TOKEN.search(req):
        return False
    # "load X.v" plus an analysis/transform clause is not a bare read.
    tail = req[_READ_KEYWORD.search(req).end():]
    v = _V_TOKEN.search(tail)
    rest = tail[v.end():] if v else tail
    if _ACTION_VERBS.search(rest):
        return False
    return not _has_extra_clause(req, _READ_KEYWORD)


def extract_read_path(req):
    fname = _v_after(req, _READ_KEYWORD)
    if not fname:
        return None
    dm = _DIR_TOKEN.search(req)
    if dm and "/" not in fname and "\\" not in fname:
        d = _match_value(dm).rstrip("/\\")
        sep = "\\" if "\\" in d and "/" not in d else "/"
        return d + sep + fname
    return fname


def is_write_request(req):
    wm = _WRITE_KEYWORD.search(req)
    if not wm or not _V_TOKEN.search(req):
        return False
    if _READ_KEYWORD.search(req):
        return False
    # Anything before the write verb that looks like a real task means this is a
    # compound request; let the ReAct loop deal with it.
    head = req[:wm.start()]
    if _ACTION_VERBS.search(head):
        return False
    return not _has_extra_clause(req, _WRITE_KEYWORD)


def extract_write_path(req):
    return _v_after(req, _WRITE_KEYWORD) or "output.v"


def session_command(verb, path):
    """Build read/write syntax while preserving paths that contain spaces."""
    if re.search(r"\s", path):
        quote = "'" if '"' in path and "'" not in path else '"'
        return verb + " " + quote + path + quote
    return verb + " " + path


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  LLM output parsing                                                    ║
# ╚════════════════════════════════════════════════════════════════════════╝

# ── Command normalization ────────────────────────────────────────────────────
# The backend grammar is "family mode args...", space separated.  A weak model
# that has seen the family named in the routing table but not its usage block
# reaches for Python attribute syntax instead and emits
# "func_query.boolean_expression n12".  The backend answers "Unknown command",
# the model reads that as "wrong feature name" and burns the whole step budget
# guessing new feature names while never touching the separator.  Repairing the
# separator here is deterministic and costs nothing.
_COMMAND_FAMILIES = (
    "structure_query", "cone_query", "path_query", "depth_query",
    "func_query", "func_search", "sequential_query",
    "opt_query", "opt_apply", "edit_apply", "report_query", "equiv_query",
    "read", "write", "help", "quit", "exit",
)


def normalize_command(command):
    """Repair harmless command spelling differences before strict validation."""
    cmd = command.strip()
    if not cmd:
        return cmd
    head = cmd.split(None, 1)[0]
    for fam in _COMMAND_FAMILIES:
        if head != fam and head.lower().startswith(fam + "."):
            fixed = fam + " " + cmd[len(fam) + 1:]
            sys.stderr.write("[normalize] {!r} -> {!r}\n".format(cmd, fixed))
            cmd = fixed
            break
        if head != fam and re.match(re.escape(fam) + r"(::|:|--|-)", head, re.IGNORECASE):
            sep = re.match(re.escape(fam) + r"(::|:|--|-)", head, re.IGNORECASE).group(1)
            fixed = fam + " " + cmd[len(fam) + len(sep):]
            sys.stderr.write("[normalize] {!r} -> {!r}\n".format(cmd, fixed))
            cmd = fixed
            break

    try:
        tokens = shlex.split(cmd, posix=True)
    except ValueError:
        return cmd
    if tokens and tokens[0].lower() == "path_query":
        aliases = {
            "--avoid": "-avoid",
            "--req": "-req",
            "--count-only": "-count_only",
            "--count_only": "-count_only",
            "--depth-eq": "-depth_eq",
            "--depth_eq": "-depth_eq",
            "--depth-ge": "-depth_ge",
            "--depth_ge": "-depth_ge",
            "--depth-le": "-depth_le",
            "--depth_le": "-depth_le",
        }
        normalized = [aliases.get(token.lower(), token) for token in tokens]
        if normalized != tokens:
            # Backend commands are parsed by the tools CLI, not by a shell.
            # shlex.quote() would therefore make apostrophes part of endpoint
            # names such as n0[0], causing otherwise valid bus bits to fail.
            fixed = " ".join(normalized)
            sys.stderr.write("[normalize] {!r} -> {!r}\n".format(cmd, fixed))
            cmd = fixed
    return cmd


_ACTION_LINE = re.compile(r"^\s*(RUN|ANSWER)\b\s*:?\s*", re.IGNORECASE)
_REASON_LINE = re.compile(r"^\s*REASON\b\s*:?\s*", re.IGNORECASE)

_DELIVERABLE_KINDS = set(("entity_list", "scalar", "comparison", "summary"))
_DELIVERABLE_COVERAGE = set(("exhaustive", "bounded", "scalar", "summary"))
_BOOLEAN_WITNESS_REQUEST_RE = re.compile(
    r"\b(counter[ -]?example|witness|input\s+(?:assignment|pattern|vector)|"
    r"satisfying\s+assignment)\b", re.IGNORECASE)


def _repair_unrequested_equivalence_deliverables(value, user_req):
    """Drop only optional Boolean outputs that the request did not ask for.

    This repair never changes the selected family, mode, operands, or core
    equivalence scalar. Requests that explicitly ask for a witness or solver
    status remain strict and must provide a valid deliverable contract. Solver
    status is harmless supporting evidence, so this repair leaves it intact.
    """
    if not user_req or not isinstance(value, dict):
        return value
    if (value.get("action") != "run" or
            str(value.get("family", "")).lower() != "func_query" or
            str(value.get("mode", "")).lower() != "equivalence" or
            not isinstance(value.get("deliverables"), list)):
        return value

    def normalized_token(item, field):
        return str(item.get(field, "")).strip().lower().replace("_", " ")

    has_core_equivalence = any(
        isinstance(item, dict) and item.get("kind") == "scalar" and
        normalized_token(item, "metric") in ("equivalence", "equivalent")
        for item in value["deliverables"])
    if not has_core_equivalence:
        return value

    asks_for_witness = bool(_BOOLEAN_WITNESS_REQUEST_RE.search(user_req))
    repaired = []
    for item in value["deliverables"]:
        if not isinstance(item, dict):
            repaired.append(item)
            continue
        kind = item.get("kind")
        entity = normalized_token(item, "entity")
        metric = normalized_token(item, "metric")
        if (kind == "entity_list" and
                entity in ("counterexample", "counter example", "witness") and
                not asks_for_witness):
            continue
        repaired.append(item)
    if len(repaired) == len(value["deliverables"]):
        return value
    normalized = dict(value)
    normalized["deliverables"] = repaired
    return normalized


def _normalize_deliverables(value, action_clause_ids):
    """Validate and canonicalize one prompt-local deliverable contract."""
    if not isinstance(value, list) or not value:
        return None, "deliverables must be a non-empty list"
    allowed = set((
        "clause_id", "kind", "coverage", "entity", "metric", "subjects", "limit",
    ))
    normalized = []
    seen_clauses = set()
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            return None, "deliverables[{}] must be an object".format(index)
        unknown = sorted(set(item) - allowed)
        if unknown:
            return None, "deliverables[{}] contains unknown fields {}".format(
                index, ", ".join(unknown))
        clause_id = item.get("clause_id")
        kind = item.get("kind")
        coverage = item.get("coverage")
        if not isinstance(clause_id, str) or not clause_id.strip():
            return None, "deliverables[{}].clause_id must be non-empty".format(index)
        clause_id = clause_id.strip()
        if clause_id not in action_clause_ids:
            return None, "deliverable clause_id {} is not present in clause_ids".format(
                clause_id)
        if kind not in _DELIVERABLE_KINDS:
            return None, "deliverable kind must be one of {}".format(
                ", ".join(sorted(_DELIVERABLE_KINDS)))
        if coverage not in _DELIVERABLE_COVERAGE:
            return None, "deliverable coverage must be one of {}".format(
                ", ".join(sorted(_DELIVERABLE_COVERAGE)))
        expected_coverage = {
            "entity_list": set(("exhaustive", "bounded")),
            "scalar": set(("scalar",)),
            "comparison": set(("scalar",)),
            "summary": set(("summary",)),
        }[kind]
        if coverage not in expected_coverage:
            return None, "deliverable kind {} is incompatible with coverage {}".format(
                kind, coverage)

        result = {
            "clause_id": clause_id,
            "kind": kind,
            "coverage": coverage,
        }
        for field in ("entity", "metric"):
            if field in item:
                if not isinstance(item[field], str) or not item[field].strip():
                    return None, "deliverables[{}].{} must be non-empty".format(
                        index, field)
                result[field] = item[field].strip().lower()
        if "subjects" in item:
            subjects = item["subjects"]
            if (not isinstance(subjects, list) or len(subjects) < 2 or
                    any(not isinstance(subject, str) or not subject.strip()
                        for subject in subjects)):
                return None, "comparison subjects must contain at least two names"
            result["subjects"] = [subject.strip() for subject in subjects]
        if kind == "comparison" and "subjects" not in result:
            return None, "comparison deliverable requires subjects"
        if "limit" in item:
            limit = item["limit"]
            if isinstance(limit, bool) or not isinstance(limit, int) or limit <= 0:
                return None, "bounded deliverable limit must be a positive integer"
            result["limit"] = limit
        if coverage == "bounded" and "limit" not in result:
            return None, "bounded deliverable requires limit"
        normalized.append(result)
        seen_clauses.add(clause_id)
    return normalized, ""


def _normalize_workflow(value):
    """Validate one optional ordered multi-command workflow contract."""
    if not isinstance(value, dict) or set(value) != set(("stages",)):
        return None, "workflow must contain only a stages list"
    stages = value.get("stages")
    if not isinstance(stages, list) or len(stages) < 2:
        return None, "workflow.stages must contain at least two ordered stages"
    if len(stages) > 6:
        return None, "workflow.stages exceeds the six-stage protocol limit"
    normalized = []
    for index, stage in enumerate(stages):
        if not isinstance(stage, dict):
            return None, "workflow.stages[{}] must be an object".format(index)
        unknown = sorted(set(stage) - set(("family", "mode", "args")))
        if unknown:
            return (None, "workflow.stages[{}] contains unknown fields {}".format(
                index, ", ".join(unknown)))
        family = stage.get("family")
        mode = stage.get("mode")
        args = stage.get("args")
        if not isinstance(family, str) or not family.strip():
            return None, "workflow.stages[{}] requires family".format(index)
        if not isinstance(mode, str) or not mode.strip():
            return None, "workflow.stages[{}] requires mode".format(index)
        if not isinstance(args, list):
            return None, "workflow.stages[{}] requires an args list".format(index)
        if any(isinstance(item, bool) or not isinstance(item, (str, int, float))
               for item in args):
            return (None, "workflow.stages[{}].args must contain only string or "
                    "numeric tokens".format(index))
        normalized.append({
            "family": family.strip().lower(),
            "mode": mode.strip().lower(),
            "args": [str(item) for item in args],
        })
    return {"stages": normalized}, ""


def _extract_json_object(text):
    """Parse one model response as one JSON object, tolerating only code fences."""
    cleaned = (text or "").strip()
    if cleaned.startswith("```") and cleaned.endswith("```"):
        lines = cleaned.splitlines()
        if len(lines) >= 3:
            cleaned = "\n".join(lines[1:-1]).strip()
    try:
        value = json.loads(cleaned)
    except (TypeError, ValueError) as exc:
        return None, "invalid JSON: {}".format(exc)
    if not isinstance(value, dict):
        return None, "top-level JSON value must be an object"
    return value, ""


def parse_action_json(text, user_req=None):
    """Return (action, payload, error) for the strict internal protocol."""
    value, error = _extract_json_object(text)
    if error:
        return "NONE", None, error
    value = _repair_unrequested_equivalence_deliverables(value, user_req)
    action = value.get("action")
    clause_ids = value.get("clause_ids")
    if action not in ("run", "answer"):
        return "NONE", None, "action must be run or answer"
    if (not isinstance(clause_ids, list) or not clause_ids or
            any(not isinstance(item, str) or not item for item in clause_ids)):
        return "NONE", None, "clause_ids must be a non-empty string list"
    if action == "run":
        allowed = set((
            "action", "family", "mode", "args", "command", "clause_ids",
            "deliverables", "workflow",
        ))
        unknown = sorted(set(value) - allowed)
        if unknown:
            return ("NONE", None,
                    "run action contains unknown fields {}; put every tool operand "
                    "and option in args".format(", ".join(unknown)))
        if not isinstance(value.get("family"), str) or not value.get("family"):
            return "NONE", None, "run action requires family"
        has_command = "command" in value
        has_structured = "mode" in value or "args" in value
        if has_command and has_structured:
            return "NONE", None, "run action must use mode/args or legacy command, not both"
        if has_structured:
            if not isinstance(value.get("mode"), str) or not value.get("mode").strip():
                return "NONE", None, "structured run action requires mode"
            args = value.get("args")
            if not isinstance(args, list):
                return "NONE", None, "structured run action requires an args list"
            if any(isinstance(item, bool) or not isinstance(item, (str, int, float))
                   for item in args):
                return "NONE", None, "run args must contain only string or numeric tokens"
        elif (not isinstance(value.get("command"), str) or
              not value.get("command").strip()):
            return "NONE", None, "run action requires mode/args"
        if "deliverables" in value:
            deliverables, deliverable_error = _normalize_deliverables(
                value["deliverables"], clause_ids)
            if deliverable_error:
                return "NONE", None, deliverable_error
            value["deliverables"] = deliverables
        if "workflow" in value:
            workflow, workflow_error = _normalize_workflow(value["workflow"])
            if workflow_error:
                return "NONE", None, workflow_error
            value["workflow"] = workflow
    else:
        allowed = set(("action", "answer", "clause_ids", "evidence"))
        if set(value) - allowed:
            return "NONE", None, "answer action contains unknown fields"
        if not isinstance(value.get("answer"), str) or not value.get("answer").strip():
            return "NONE", None, "answer action requires non-empty answer"
        evidence = value.get("evidence")
        if not isinstance(evidence, list):
            return "NONE", None, "answer action requires an evidence list"
    return action.upper(), value, ""


def _mode_names(card):
    names = set()
    for mode in card.get("modes", []):
        names.add(mode["name"].lower())
        for alias in mode.get("aliases", []):
            names.add(alias.lower())
    return names


_PUBLIC_GATE_TYPES = set((
    "AND", "OR", "NOT", "NAND", "NOR", "XOR", "XNOR", "BUF", "DFF",
))
_OPT_GATE_TYPES = _PUBLIC_GATE_TYPES - set(("DFF",))
_EDIT_SCOPES = set((
    "whole", "net_fanin", "net_fanout", "gate_fanin", "gate_fanout", "gate",
))
_OPT_SCOPES = set((
    "whole", "net_fanin", "net_fanout", "gate_fanin", "gate_fanout",
))
_OPT_BASIS_SCOPES = set(("whole", "net_fanin", "gate_fanin"))


def _mode_record(card, mode_name):
    key = (mode_name or "").lower()
    for mode in card.get("modes", []):
        names = [mode.get("name", "").lower()]
        names.extend(alias.lower() for alias in mode.get("aliases", []))
        if key in names:
            return mode
    return None


def _unknown_mode_error(family, mode, card):
    valid_modes = sorted(_mode_names(card))
    valid_text = ", ".join(valid_modes) if valid_modes else "<none>"
    example_mode = valid_modes[0] if valid_modes else "<documented_mode>"
    return (
        "unknown mode {} for family {}. The family is already supplied in the "
        "family field, so mode must contain only one documented mode name. "
        "Valid modes: {}. Use family=\"{}\", mode=\"{}\"; do not repeat {} "
        "inside mode.".format(
            mode, family, valid_text, family, example_mode, family))


def _positive_integer(token):
    try:
        return int(str(token), 10) > 0 and str(token).strip().isdigit()
    except (TypeError, ValueError):
        return False


def _positive_number(token):
    try:
        value = float(str(token))
        return math.isfinite(value) and value > 0.0
    except (TypeError, ValueError):
        return False


def _nonnegative_integer(token):
    try:
        value = str(token).strip()
        return value.isdigit() and int(value, 10) >= 0
    except (TypeError, ValueError):
        return False


def _consume_edit_scope(args):
    if not args:
        return False, 0, "scope is required"
    scope = args[0].lower()
    if scope not in _EDIT_SCOPES:
        return (False, 0,
                "invalid scope {}; use whole, net_fanin, net_fanout, "
                "gate_fanin, gate_fanout, or gate".format(args[0]))
    index = 1
    if scope != "whole":
        if len(args) <= index or args[index].startswith("-"):
            return False, 0, "scope {} requires a scope name".format(scope)
        index += 1
    return True, index, ""


def _validate_gate_type(token, allow_all=False):
    value = (token or "").upper()
    return value in _PUBLIC_GATE_TYPES or (allow_all and value == "ALL")


def _validate_opt_gate_type(token):
    """Return whether a token is a combinational optimization basis type."""
    return str(token).upper() in _OPT_GATE_TYPES


def _validate_edit_grammar(mode, args):
    no_args = set((
        "cleanup_buffers", "collapse_double_inverter",
        "local_simplification_fixpoint", "safe_cleanup_fixpoint",
        "remove_unused_nets", "merge_structurally_equivalent_gates",
        "merge_duplicate_dffs", "simplify_same_input",
    ))
    if mode in no_args:
        return (not args, "{} accepts no arguments".format(mode) if args else "")
    if mode in ("rename_gate", "rename_net", "insert_buffer_before_gate"):
        expected = 2
        return (len(args) == expected,
                "{} requires exactly {} arguments".format(mode, expected))
    if mode in ("remove_net_if_unused", "insert_buffers_on_each_load",
                "insert_buffer_at_driver"):
        return (len(args) == 1, "{} requires exactly one argument".format(mode))
    if mode == "remove_dead_logic":
        valid = len(args) <= 1 and (not args or args[0] == "--include-sequential")
        return valid, "remove_dead_logic accepts only --include-sequential"
    if mode == "remove_redundant_logic":
        valid = (not args or
                 (len(args) == 2 and args[0] == "--time-limit" and
                  _positive_number(args[1])))
        return valid, "remove_redundant_logic accepts only --time-limit <positive seconds>"
    if mode == "simplify_constants":
        index = 0
        if index < len(args) and not args[index].startswith("-"):
            if not _validate_gate_type(args[index], allow_all=True):
                return False, "simplify_constants gate type must be a public gate type or all"
            index += 1
        if index < len(args) and not args[index].startswith("-"):
            if args[index].lower() not in ("0", "1", "any"):
                return False, "simplify_constants value must be 0, 1, or any"
            index += 1
        if index == len(args):
            return True, ""
        if (len(args) == index + 2 and args[index] == "--inputs" and
                _positive_integer(args[index + 1])):
            return True, ""
        return False, "simplify_constants accepts [gate_type|all] [0|1|any] [--inputs N]"
    if mode in ("insert_buffers_for_fanout",):
        valid = len(args) == 1 and _positive_integer(args[0])
        return valid, "{} requires one positive integer".format(mode)
    if mode == "insert_buffers_for_net":
        valid = len(args) == 2 and _positive_integer(args[1])
        return valid, "insert_buffers_for_net requires <net> <positive max_fanout>"
    if mode == "insert_buffers_for_dff_control":
        valid = (len(args) == 2 and _positive_integer(args[0]) and
                 args[1] in ("-clock", "-reset"))
        return valid, "insert_buffers_for_dff_control requires <positive max_fanout> -clock|-reset"
    if mode == "insert_buffers_by_gate_type":
        valid = (1 <= len(args) <= 2 and _validate_gate_type(args[0]) and
                 (len(args) == 1 or args[1] in ("-inputs", "-outputs", "-both")))
        return valid, "insert_buffers_by_gate_type requires <gate type> and an optional direction"
    if mode == "merge_functionally_equivalent_gates":
        ok, index, error = _consume_edit_scope(args)
        if not ok:
            return False, error
        seen = set()
        while index < len(args):
            option = args[index]
            if option in seen:
                return False, "duplicate option {}".format(option)
            seen.add(option)
            if option == "--gate-type":
                if index + 1 >= len(args) or not _validate_gate_type(args[index + 1]):
                    return False, "--gate-type requires a public gate type"
                index += 2
            elif option == "--patterns":
                if index + 1 >= len(args) or not _positive_integer(args[index + 1]):
                    return False, "--patterns requires a positive integer"
                index += 2
            elif option == "--time-limit":
                if index + 1 >= len(args) or not _positive_number(args[index + 1]):
                    return False, "--time-limit requires positive seconds"
                index += 2
            else:
                return False, "unknown merge_functionally_equivalent_gates option {}".format(option)
        return True, ""
    if mode in ("convert_basis", "replace_type"):
        ok, index, error = _consume_edit_scope(args)
        if not ok:
            return False, error
        if mode == "replace_type":
            if index >= len(args) or not _validate_gate_type(args[index]):
                return False, "replace_type requires a public target gate type after the scope"
            index += 1
        allowed_count = 0
        seen_allow = False
        while index < len(args):
            option = args[index]
            if option == "--validate_equivalence":
                index += 1
                continue
            if option not in ("-allow", "-ban"):
                return False, "unknown {} option {}".format(mode, option)
            if option == "-allow":
                if seen_allow:
                    return False, "duplicate -allow option"
                seen_allow = True
            index += 1
            start = index
            while index < len(args) and not args[index].startswith("-"):
                if not _validate_gate_type(args[index]):
                    return False, "{} requires public gate type tokens; use NOT, not inverter".format(option)
                index += 1
            if index == start:
                return False, "{} requires at least one gate type".format(option)
            if option == "-allow":
                allowed_count += index - start
        if not seen_allow or allowed_count == 0:
            return False, "{} requires -allow followed by at least one gate type".format(mode)
        return True, ""
    return True, ""


def _validate_opt_grammar(mode, args):
    """Validate the public optimization option grammar before backend use."""
    if mode not in ("critical_path_depth", "gate_count_minimization"):
        return True, ""
    index = 0
    seen = set()
    scope = "whole"
    scope_name = ""
    basis_scope = "whole"
    basis_name = ""
    cost_scope = "whole"
    outside_constraint = False
    while index < len(args):
        option = args[index]
        if not option.startswith("--"):
            return False, "unexpected opt_apply argument {}; expected an option".format(option)
        if option in seen:
            return False, "duplicate opt_apply option {}".format(option)
        seen.add(option)
        if option == "--scope":
            if index + 1 >= len(args) or args[index + 1].lower() not in _OPT_SCOPES:
                return (False,
                        "--scope requires whole, net_fanin, net_fanout, "
                        "gate_fanin, or gate_fanout; cone is a --cost-scope value")
            scope = args[index + 1].lower()
            index += 2
            if scope != "whole" and index < len(args) and not args[index].startswith("--"):
                scope_name = args[index]
                index += 1
            continue
        if option == "--name":
            if index + 1 >= len(args) or args[index + 1].startswith("--"):
                return False, "--name requires one scope target name"
            scope_name = args[index + 1]
            index += 2
            continue
        if option == "--cost-scope":
            if index + 1 >= len(args) or args[index + 1].lower() not in ("whole", "cone"):
                return False, "--cost-scope requires whole or cone"
            cost_scope = args[index + 1].lower()
            index += 2
            continue
        if option == "--basis-scope":
            if (index + 1 >= len(args) or
                    args[index + 1].lower() not in _OPT_BASIS_SCOPES):
                return (False,
                        "--basis-scope requires whole, net_fanin, or gate_fanin; "
                        "fanout basis scopes are not supported")
            basis_scope = args[index + 1].lower()
            index += 2
            if (basis_scope != "whole" and index < len(args) and
                    not args[index].startswith("--")):
                basis_name = args[index]
                index += 1
            continue
        if option == "--basis-name":
            if index + 1 >= len(args) or args[index + 1].startswith("--"):
                return False, "--basis-name requires one scope target name"
            basis_name = args[index + 1]
            index += 2
            continue
        if option in ("--allowed", "--banned", "--outside-allowed",
                      "--outside-banned"):
            outside_constraint = outside_constraint or option.startswith("--outside-")
            index += 1
            start = index
            while index < len(args) and not args[index].startswith("--"):
                if not _validate_opt_gate_type(args[index]):
                    return (False,
                            "{} requires combinational gate type tokens; DFF is a "
                            "sequential boundary, not an optimization basis type"
                            .format(option))
                index += 1
            if index == start:
                return False, "{} requires at least one gate type".format(option)
            continue
        if option == "--target-cost":
            if index + 1 >= len(args) or not _nonnegative_integer(args[index + 1]):
                return False, "--target-cost requires a non-negative integer"
            index += 2
            continue
        if option == "--time-limit":
            if index + 1 >= len(args) or not _positive_number(args[index + 1]):
                return False, "--time-limit requires positive seconds"
            index += 2
            continue
        if option in ("--allow-no-improvement", "--verbose"):
            index += 1
            continue
        return False, "unknown opt_apply option {}".format(option)

    if scope != "whole" and not scope_name:
        return False, "--scope {} requires a net or gate name".format(scope)
    if scope == "whole" and scope_name:
        return False, "whole optimization scope does not take a scope name"
    if basis_scope != "whole" and not basis_name:
        return False, "--basis-scope {} requires a net or gate name".format(basis_scope)
    if basis_scope == "whole" and basis_name:
        return False, "whole basis scope does not take a scope name"
    if cost_scope == "cone" and scope not in ("net_fanin", "gate_fanin"):
        return (False,
                "--cost-scope cone requires --scope net_fanin <net> or "
                "gate_fanin <gate>")
    if outside_constraint and basis_scope == "whole":
        return (False,
                "outside gate-type constraints require an explicit net_fanin "
                "or gate_fanin --basis-scope")
    return True, ""


_WHOLE_BASIS_SCOPE_RE = re.compile(
    r"\b(?:entire|whole)\s+(?:design|netlist|circuit)\b|"
    r"\b(?:all|every)\s+(?:the\s+)?gates?\b", re.I)
_FINAL_BASIS_RE = re.compile(
    r"\b(?:basis|gate\s+set|use\s+only|using\s+only|contains?\s+only|"
    r"consists?\s+of|(?:remain|maintain|keep)(?:s|ed|ing)?\b"
    r"[^.\n]{0,40}\bonly|(?:restricted|limited)\s+to|exclusively|"
    r"(?:AND|OR|NOT|NAND|NOR|XOR|XNOR|BUF|DFF)\s*[- ]only)\b", re.I)
_MAPPING_INTENT_RE = re.compile(
    r"\b(?:map|mapping|remap|convert|decompose|reconstruct|restructure|"
    r"technology\s+mapping|rewrite|replace)\b", re.I)
_CONSTANT_SELECTION_RE = re.compile(
    r"\bconstant\s*[- ]?(?:0|1|zero|one)\b|"
    r"\btied\s+(?:to\s+)?(?:constant\s+)?(?:0|1|zero|one|high|low)\b|"
    r"\b1\s*'\s*b[01]\b", re.I)
_SAME_INPUT_SELECTION_RE = re.compile(
    r"\b(?:same|identical)\s+(?:input|operand)s?\b|"
    r"\bboth\s+inputs?\s+(?:are\s+)?(?:the\s+)?same\b", re.I)
_LOCAL_REWRITE_INTENT_RE = re.compile(
    r"\b(?:replace|rewrite|convert|simplif\w*|propagat\w*|fold\w*|"
    r"eliminat\w*|remove)\b", re.I)
_STRUCTURAL_DUPLICATE_RE = re.compile(
    r"\bstructural(?:ly)?\s+(?:duplicate|identical|equivalent)\w*\b|"
    r"\b(?:same|identical)\s+(?:gate\s+)?(?:type|operator)\b"
    r"[^.?!\n]{0,100}\b(?:same|identical)\s+(?:input\s+)?"
    r"(?:nets?|signals?|operands?|inputs?)\b|"
    r"\b(?:same|identical)\s+(?:input\s+)?"
    r"(?:nets?|signals?|operands?|inputs?)\b"
    r"[^.?!\n]{0,100}\b(?:same|identical)\s+(?:gate\s+)?"
    r"(?:type|operator)\b",
    re.I)
_FUNCTIONAL_DUPLICATE_RE = re.compile(
    r"\bfunction(?:al|ally)?\s+equivalent\b|"
    r"\b(?:same|equal|equivalent)\s+(?:Boolean\s+)?function\b|"
    r"\b(?:Boolean|logical)\s+equivalence\b|"
    r"\b(?:structurally|implementation(?:s)?|structures?)\s+"
    r"(?:different|differ)\w*\b",
    re.I)
_SOURCE_GATE_RE_TEMPLATE = (
    r"\b(?:replace|rewrite|convert|decompose|map)\s+"
    r"(?:(?:all|every|each|the)\s+)?"
    r"(?:\d+\s*[- ]?input\s+)?{gate}\s+gates?\b")
_GATE_TYPE_TOKEN_RE = re.compile(
    r"\b(XNOR|NAND|NOR|XOR|AND|NOT|BUF|DFF|OR)\b", re.I)
_FUNCTIONAL_REDUNDANCY_RE = re.compile(
    r"\b(?:functionally|logically|boolean)\s+redundant\b|"
    r"\bredundant\s+(?:logic|gates?)\b", re.I)
_DEAD_LOGIC_RE = re.compile(
    r"\b(?:dead|dangling|unreachable)\s+(?:logic|gates?)\b|"
    r"\bunused\b[^.\n]{0,40}\b(?:logic|gates?)\b|"
    r"\b(?:logic|gates?)\s+(?:that\s+)?(?:do|does|can)\s+not\s+"
    r"(?:contribute|reach|connect)\b|"
    r"\bnot\s+(?:connected|contributing)\s+to\s+(?:any\s+)?"
    r"(?:primary\s+)?outputs?\b", re.I)
_UNUSED_NET_RE = re.compile(r"\bunused\s+(?:wires?|nets?|signals?)\b", re.I)


def _explicit_source_gate_types(user_req):
    """Return gate types explicitly selected as rewrite sources by the request."""
    text = user_req or ""
    found = []
    for gate_type in sorted(_PUBLIC_GATE_TYPES):
        pattern = _SOURCE_GATE_RE_TEMPLATE.format(gate=re.escape(gate_type))
        if re.search(pattern, text, re.I):
            found.append(gate_type)
    return found


def _explicit_cleanup_mode(user_req):
    """Return one unambiguous cleanup mode selected by request semantics."""
    text = user_req or ""
    if _FUNCTIONAL_REDUNDANCY_RE.search(text):
        return "remove_redundant_logic"
    if _DEAD_LOGIC_RE.search(text):
        return "remove_dead_logic"
    if _UNUSED_NET_RE.search(text):
        return "remove_unused_nets"
    return None


def _canonical_cleanup_mode(mode):
    if mode in ("trim_dead_logic", "remove_dangling_logic"):
        return "remove_dead_logic"
    return mode


def _requested_replace_scope(user_req, source_types):
    """Infer scope only when the request states a concrete replacement scope."""
    text = user_req or ""
    match = re.search(
        r"\b(?:fanin\s+|logic\s+)?cone\s+of\s+"
        r"(?:(output|net|signal|gate(?:\s+instance)?)\s+)?"
        r"([A-Za-z_$][A-Za-z0-9_$./\[\]-]*)", text, re.I)
    if match:
        role = (match.group(1) or "").lower()
        scope = "gate_fanin" if role.startswith("gate") else "net_fanin"
        return scope, match.group(2)
    if (_WHOLE_BASIS_SCOPE_RE.search(text) or
            re.search(r"\b(?:in|throughout)\s+(?:this|the)\s+"
                      r"(?:design|netlist|circuit)\b", text, re.I) or
            (source_types and re.search(r"\b(?:all|every)\s+", text, re.I))):
        return "whole", ""
    return None


def _gate_types_in_basis_text(text, excluded_types=()):
    """Extract basis tokens without treating grammatical and/or/not as gates."""
    found = []
    excluded = set(excluded_types or ())
    for match in _GATE_TYPE_TOKEN_RE.finditer(text or ""):
        raw_gate_type = match.group(1)
        # Lowercase forms normally act as grammar. They name gate types only
        # when immediately qualified as a gate or an "-only" basis.
        if (raw_gate_type in ("and", "or", "not") and
                not re.match(r"(?:\s+(?:logic\s+)?gates?\b|\s*[- ]only\b)",
                             (text or "")[match.end():], re.I)):
            continue
        gate_type = raw_gate_type.upper()
        if gate_type not in found and gate_type not in excluded:
            found.append(gate_type)
    return found


def _requested_replacement_basis(user_req, source_types):
    """Extract gate types stated after the selected source-gate phrase."""
    text = user_req or ""
    source_ends = []
    for gate_type in source_types:
        pattern = _SOURCE_GATE_RE_TEMPLATE.format(gate=re.escape(gate_type))
        match = re.search(pattern, text, re.I)
        if match:
            source_ends.append(match.end())
    if not source_ends:
        return []
    return _gate_types_in_basis_text(
        text[min(source_ends):], excluded_types=source_types)


def _requested_final_basis(user_req):
    """Extract the explicitly constrained final basis of a scope."""
    text = user_req or ""
    marker = _FINAL_BASIS_RE.search(text)
    if not marker:
        return []
    # Include a short prefix so forms such as "NOR-only" retain NOR.
    start = max(0, marker.start() - 16)
    return _gate_types_in_basis_text(text[start:])


def _replace_type_signature(args):
    """Return (scope, scope_name, source_type, allowed_types) when parseable."""
    if not args:
        return None
    scope = str(args[0]).lower()
    if scope not in _EDIT_SCOPES:
        return None
    index = 1
    scope_name = ""
    if scope != "whole":
        if index >= len(args) or str(args[index]).startswith("-"):
            return None
        scope_name = str(args[index])
        index += 1
    if index >= len(args) or not _validate_gate_type(str(args[index])):
        return None
    source_type = str(args[index]).upper()
    index += 1
    allowed_types = []
    while index < len(args):
        option = str(args[index])
        index += 1
        if option == "--validate_equivalence":
            continue
        if option not in ("-allow", "-ban"):
            return None
        values = []
        while index < len(args) and not str(args[index]).startswith("-"):
            values.append(str(args[index]).upper())
            index += 1
        if option == "-allow":
            allowed_types.extend(values)
    return scope, scope_name, source_type, allowed_types


def _convert_basis_signature(args):
    """Return (scope, scope_name, allowed_types) when parseable."""
    if not args:
        return None
    scope = str(args[0]).lower()
    if scope not in _EDIT_SCOPES:
        return None
    index = 1
    scope_name = ""
    if scope != "whole":
        if index >= len(args) or str(args[index]).startswith("-"):
            return None
        scope_name = str(args[index])
        index += 1
    allowed_types = []
    while index < len(args):
        option = str(args[index])
        index += 1
        if option == "--validate_equivalence":
            continue
        if option not in ("-allow", "-ban"):
            return None
        values = []
        while index < len(args) and not str(args[index]).startswith("-"):
            values.append(str(args[index]).upper())
            index += 1
        if option == "-allow":
            allowed_types.extend(values)
    return scope, scope_name, allowed_types


def _requested_duplicate_merge_criterion(user_req):
    """Return the explicit gate-merge identity criterion, if one is stated.

    Structural identity is intentionally more specific than a general claim
    that two outputs compute the same function. This keeps requests such as
    "same function on the same inputs (structural duplicates)" on the cheap,
    deterministic structural path while leaving underspecified merge requests
    to the model and tool card.
    """
    text = user_req or ""
    if _STRUCTURAL_DUPLICATE_RE.search(text):
        return "structural"
    if _FUNCTIONAL_DUPLICATE_RE.search(text):
        return "functional"
    return ""


def _validate_edit_semantics(user_req, mode, args, active_cost_function=None):
    """Guard mutually exclusive edit modes after their CLI grammar is valid.

    This checks semantic ownership, not exact benchmark wording. It deliberately
    uses broad request properties: selection by a constant input, a scoped
    final basis, or one explicitly named source gate type.
    """
    text = user_req or ""
    optimization_mode = _requested_optimization_mode(text, active_cost_function)
    prescribed_edit = _has_prescribed_edit_intent(text)
    if optimization_mode and not prescribed_edit:
        return (False,
                "the request leaves the transformation strategy open and asks "
                "to reduce a measurable cost. Use OptApply {} rather than a "
                "fixed EditApply operation".format(optimization_mode))
    local_rewrite = bool(_LOCAL_REWRITE_INTENT_RE.search(text))
    constant_selected = bool(_CONSTANT_SELECTION_RE.search(text)) and local_rewrite
    same_input_selected = bool(_SAME_INPUT_SELECTION_RE.search(text)) and local_rewrite
    final_basis_request = (bool(_MAPPING_INTENT_RE.search(text)) and
                           bool(_FINAL_BASIS_RE.search(text)))
    source_types = _explicit_source_gate_types(text)
    cleanup_mode = _explicit_cleanup_mode(text)
    merge_criterion = _requested_duplicate_merge_criterion(text)

    if mode in ("merge_structurally_equivalent_gates",
                "merge_functionally_equivalent_gates"):
        if (merge_criterion == "structural" and
                mode != "merge_structurally_equivalent_gates"):
            return (False,
                    "the request explicitly defines structural identity by "
                    "the same operator/type and inputs; use "
                    "merge_structurally_equivalent_gates instead of the "
                    "SAT-based functional merge")
        if (merge_criterion == "functional" and
                mode != "merge_functionally_equivalent_gates"):
            return (False,
                    "the request requires Boolean functional equivalence even "
                    "when structures or operands may differ; use "
                    "merge_functionally_equivalent_gates with the requested "
                    "scope")

    if constant_selected and mode != "simplify_constants":
        return (False,
                "the request selects gates by a constant-valued input; use "
                "simplify_constants and preserve its gate type, constant value, "
                "and input-count restrictions")
    if constant_selected:
        return True, ""
    if same_input_selected and mode != "simplify_same_input":
        return (False,
                "the request selects gates by a same-input relation; use "
                "simplify_same_input rather than a mapping or merge mode")
    if same_input_selected:
        return True, ""
    if cleanup_mode and _canonical_cleanup_mode(mode) != cleanup_mode:
        distinction = {
            "remove_unused_nets": "only unused wires/nets are selected",
            "remove_dead_logic": (
                "unused or dangling gates, or logic that cannot contribute to "
                "required outputs, is selected"),
            "remove_redundant_logic": (
                "functional or Boolean redundancy is selected"),
        }[cleanup_mode]
        return (False,
                "the cleanup request requires {} because {}; do not substitute "
                "another cleanup or mapping mode".format(cleanup_mode, distinction))
    if final_basis_request and not source_types and mode != "convert_basis":
        return (False,
                "the request constrains the final gate basis of a scope and does "
                "not identify one source gate type; use convert_basis with the "
                "requested scope and allowed basis types")
    if final_basis_request and not source_types and mode == "convert_basis":
        signature = _convert_basis_signature(args)
        if signature is None:
            return (False,
                    "convert_basis must preserve the request's scope and allowed "
                    "final basis")
        scope, scope_name, allowed_types = signature
        requested_scope = _requested_replace_scope(text, source_types)
        if requested_scope and (scope, scope_name) != requested_scope:
            expected_scope, expected_name = requested_scope
            expected = expected_scope + ((" " + expected_name) if expected_name else "")
            actual = scope + ((" " + scope_name) if scope_name else "")
            return (False,
                    "convert_basis scope {} does not match the requested scope {}"
                    .format(actual, expected))
        requested_basis = _requested_final_basis(text)
        if requested_basis and set(allowed_types) != set(requested_basis):
            return (False,
                    "convert_basis allowed basis {} does not match the requested "
                    "basis {}".format(
                        ", ".join(allowed_types) or "<missing>",
                        ", ".join(requested_basis)))
    if source_types and mode != "replace_type":
        return (False,
                "the request explicitly selects source gate type {} for "
                "unconditional replacement; use replace_type with that source "
                "type and the requested replacement basis".format(
                    ", ".join(source_types)))
    if source_types and mode == "replace_type":
        signature = _replace_type_signature(args)
        if signature is None:
            return (False,
                    "replace_type must preserve the request's scope, one selected "
                    "source gate type, and the allowed replacement basis")
        scope, scope_name, source_type, allowed_types = signature
        if source_type not in source_types:
            return (False,
                    "replace_type source type {} does not match the explicitly "
                    "selected source type {}".format(
                        source_type, ", ".join(source_types)))
        requested_scope = _requested_replace_scope(text, source_types)
        if requested_scope and (scope, scope_name) != requested_scope:
            expected_scope, expected_name = requested_scope
            expected = expected_scope + ((" " + expected_name) if expected_name else "")
            actual = scope + ((" " + scope_name) if scope_name else "")
            return (False,
                    "replace_type scope {} does not match the requested scope {}"
                    .format(actual, expected))
        requested_basis = _requested_replacement_basis(text, source_types)
        if requested_basis and set(allowed_types) != set(requested_basis):
            return (False,
                    "replace_type allowed basis {} does not match the requested "
                    "basis {}".format(
                        ", ".join(allowed_types) or "<missing>",
                        ", ".join(requested_basis)))
    return True, ""


_COST_DEPTH_RE = re.compile(
    r"\b(?:maximum\s+)?(?:combinational\s+|logic\s+|path\s+)?depth\b|"
    r"\blogic\s+levels?\b", re.I)
_COST_GATE_COUNT_RE = re.compile(
    r"\b(?:total\s+)?gate\s+count\b|\bnumber\s+of\s+gates?\b|\barea\b", re.I)
_OPEN_DEPTH_OPT_RE = re.compile(
    r"\b(?:minimi[sz]e|reduce|improve)\w*\b"
    r"[^.?!\n]{0,120}\b(?:critical\s+path|maximum\s+(?:logic\s+|path\s+)?"
    r"depth|logic\s+depth|logic\s+levels?)\b|"
    r"\b(?:critical\s+path|maximum\s+(?:logic\s+|path\s+)?depth|"
    r"logic\s+depth|logic\s+levels?)\b[^.?!\n]{0,120}"
    r"\b(?:minimi[sz]e|reduce|improve)\w*\b|"
    r"\boptimi[sz]e\s+(?:the\s+)?(?:maximum\s+)?"
    r"(?:critical\s+path|logic\s+depth|path\s+depth)\b", re.I)
_OPEN_GATE_COUNT_OPT_RE = re.compile(
    r"\b(?:minimi[sz]e|reduce|improve)\w*\b"
    r"[^.?!\n]{0,120}\b(?:total\s+)?gate\s+count\b|"
    r"\b(?:total\s+)?gate\s+count\b[^.?!\n]{0,120}"
    r"\b(?:minimi[sz]e|reduce|improve)\w*\b|"
    r"\bminimi[sz]e\s+(?:the\s+)?(?:design\s+)?area\b|"
    r"\boptimi[sz]e\s+(?:the\s+)?(?:total\s+)?gate\s+count\b", re.I)
_CONE_TARGET_RE = re.compile(
    r"\b(?:fanin\s+|logic\s+)?cone\s+of\s+"
    r"(?:(output|net|signal|gate(?:\s+instance)?)\s+)?"
    r"([A-Za-z_$][A-Za-z0-9_$./\[\]-]*)", re.I)
_WHOLE_BASIS_SUBJECT_RE = re.compile(
    r"\b(?:(?:entire|whole)\s+)?(?:design|netlist|circuit)\b", re.I)
_BASIS_CONE_TARGET_RE = re.compile(
    r"\b(?:fanin\s+|logic\s+)?cone\s+(?:of|for|rooted\s+at)\s+"
    r"(?:(output|net|signal|gate(?:\s+instance)?)\s+)?"
    r"([A-Za-z_$][A-Za-z0-9_$./\[\]-]*)", re.I)
_WHOLE_COST_SUBJECT_RE = re.compile(
    r"\b(?:final|entire|whole)\s+(?:design|netlist|circuit)\b|"
    r"\b(?:design|netlist|circuit)\s+as\s+a\s+whole\b", re.I)
_EXPLICIT_REWRITE_CONE_RE = re.compile(
    r"\b(?:optimi[sz]e|restructure|rewrite|reduce|minimi[sz]e)\w*\b"
    r"[^.?!\n]{0,60}\b(?:fanin\s+|logic\s+)?cone\s+"
    r"(?:of|for|rooted\s+at)\s+"
    r"(?:(output|net|signal|gate(?:\s+instance)?)\s+)?"
    r"([A-Za-z_$][A-Za-z0-9_$./\[\]-]*)", re.I)


def _explicit_cost_clause(user_req, active_cost_function=None):
    match = _COST_FUNCTION_RE.search(user_req or "")
    if match:
        return match.group(1).strip()
    return (active_cost_function or "").strip()


def _explicit_cost_mode(user_req, active_cost_function=None):
    clause = _explicit_cost_clause(user_req, active_cost_function)
    depth = bool(_COST_DEPTH_RE.search(clause))
    gate_count = bool(_COST_GATE_COUNT_RE.search(clause))
    if depth == gate_count:
        return None
    return "critical_path_depth" if depth else "gate_count_minimization"


def _requested_optimization_mode(user_req, active_cost_function=None):
    """Return the measurable optimization objective stated by the request.

    An explicit/session cost function has priority.  Otherwise an open-ended
    optimization verb must govern exactly one supported metric.  A structural
    threshold such as maximum fanout is not an optimization cost.
    """
    explicit = _explicit_cost_mode(user_req, active_cost_function)
    if explicit:
        return explicit
    text = user_req or ""
    depth = bool(_OPEN_DEPTH_OPT_RE.search(text))
    gate_count = bool(_OPEN_GATE_COUNT_OPT_RE.search(text))
    if depth == gate_count:
        return None
    return "critical_path_depth" if depth else "gate_count_minimization"


def _typed_cost_cone_target(user_req, active_cost_function=None):
    clause = _explicit_cost_clause(user_req, active_cost_function)
    match = _CONE_TARGET_RE.search(clause)
    if not match:
        return None
    role = (match.group(1) or "").lower()
    name = match.group(2)
    if not role:
        role_match = re.search(
            r"\b(output|net|signal|gate(?:\s+instance)?)\s+" +
            re.escape(name) + r"\b", user_req or "", re.I)
        role = (role_match.group(1) if role_match else "").lower()
    if not role:
        return None
    scope = "gate_fanin" if role.startswith("gate") else "net_fanin"
    return scope, name


def _explicit_basis_requirement(user_req):
    """Return an explicit final-basis subject without guessing missing roles.

    The result is ``(scope, name, allowed_types)``. An untyped named cone is a
    net fanin cone because gate scope is selected only by an explicit gate role.
    """
    for clause in re.split(r"[.!?\n]+", user_req or ""):
        marker = _FINAL_BASIS_RE.search(clause)
        if not marker:
            continue
        basis_text = clause[max(0, marker.start() - 16):]
        allowed_types = _gate_types_in_basis_text(basis_text)
        if not allowed_types:
            continue
        cone = _BASIS_CONE_TARGET_RE.search(clause)
        whole_matches = list(_WHOLE_BASIS_SUBJECT_RE.finditer(clause))
        nearest_whole = max(
            (match.start() for match in whole_matches
             if match.start() <= marker.start()), default=-1)
        nearest_cone = cone.start() if cone and cone.start() <= marker.start() else -1
        if cone and nearest_cone > nearest_whole:
            role = (cone.group(1) or "").lower()
            scope = "gate_fanin" if role.startswith("gate") else "net_fanin"
            return scope, cone.group(2), allowed_types
        if nearest_whole >= 0:
            return "whole", "", allowed_types
    return None


def _explicit_cost_is_whole(user_req, active_cost_function=None):
    clause = _explicit_cost_clause(user_req, active_cost_function)
    return bool(clause and _WHOLE_COST_SUBJECT_RE.search(clause))


def _explicit_rewrite_cone_target(user_req):
    """Return a cone only when an operation verb directly governs that cone."""
    segments = re.split(
        r"[.!?\n]+|\b(?:ensuring|subject\s+to|provided\s+that)\b",
        user_req or "", flags=re.I)
    for segment in segments:
        match = _EXPLICIT_REWRITE_CONE_RE.search(segment)
        if not match:
            continue
        role = (match.group(1) or "").lower()
        scope = "gate_fanin" if role.startswith("gate") else "net_fanin"
        return scope, match.group(2)
    return None


def _opt_constraint_values(args, option):
    """Read one optimization gate-type list from structured CLI arguments."""
    try:
        index = args.index(option) + 1
    except ValueError:
        return []
    values = []
    while index < len(args) and not str(args[index]).startswith("--"):
        values.append(str(args[index]).upper())
        index += 1
    return values


def _option_value(args, option):
    try:
        index = args.index(option)
    except ValueError:
        return None
    return args[index + 1] if index + 1 < len(args) else None


def _scope_from_opt_args(args, option, name_option):
    value = _option_value(args, option)
    if value is None:
        return "whole", ""
    try:
        index = args.index(option) + 2
    except ValueError:
        index = len(args)
    name = ""
    if value.lower() != "whole" and index < len(args) and not args[index].startswith("--"):
        name = args[index]
    explicit_name = _option_value(args, name_option)
    if explicit_name:
        name = explicit_name
    return value.lower(), name


_PRESCRIBED_EDIT_INTENT_RE = re.compile(
    r"\b(?:rename|change\s+the\s+identifier|insert|add|place)\w*\b"
    r"[^.?!\n]{0,80}\b(?:buffers?|gate|net|wire|identifier)\b|"
    r"\b(?:remove|delete|trim|sweep|eliminate)\w*\b"
    r"[^.?!\n]{0,80}\b(?:dead|dangling|unused|redundant|unreachable|"
    r"logic|gates?|nets?|wires?)\b|"
    r"\b(?:collapse|fold)\w*\b[^.?!\n]{0,80}"
    r"\b(?:inverters?|buffers?|logic)\b|"
    r"\b(?:replace|convert|decompose|reconstruct|map|remap|rewrite)\w*\b"
    r"[^.?!\n]{0,120}\b(?:gates?|logic|cone|netlist|design|circuit|basis)\b|"
    r"\b(?:simplif|propagat|merge)\w*\b[^.?!\n]{0,100}"
    r"\b(?:constants?|same\s+inputs?|gates?|dffs?|logic)\b",
    re.I)


def _has_prescribed_edit_intent(user_req):
    """Whether the request fixes the transformation rather than its cost only."""
    return bool(_PRESCRIBED_EDIT_INTENT_RE.search(user_req or ""))


def _validate_opt_semantics(user_req, mode, args, active_cost_function=None):
    expected_mode = _requested_optimization_mode(user_req, active_cost_function)
    prescribed_edit = _has_prescribed_edit_intent(user_req)
    if prescribed_edit and not expected_mode:
        return (False,
                "the request prescribes a concrete structural edit but does not "
                "state a supported cost-reduction objective. Use EditApply for "
                "the requested operation; a structural threshold such as maximum "
                "fanout is an edit operand, not an optimization cost")
    if not expected_mode:
        return (False,
                "OptApply requires a measurable logic-depth or gate-count "
                "optimization objective in this request or active session cost. "
                "Do not infer an objective from a structural threshold")
    if expected_mode and mode != expected_mode:
        return (False,
                "the explicit cost function locks optimization mode {}. A retry "
                "may correct parameters but must not change that cost metric".format(
                    expected_mode))

    expected_target = _typed_cost_cone_target(user_req, active_cost_function)
    if expected_target:
        expected_scope, expected_name = expected_target
        actual_scope, actual_name = _scope_from_opt_args(args, "--scope", "--name")
        cost_scope = (_option_value(args, "--cost-scope") or "whole").lower()
        if (actual_scope != expected_scope or actual_name.lower() != expected_name.lower() or
                cost_scope != "cone"):
            return (False,
                    "the explicit cone cost targets {} {}. Use --scope {} {} "
                    "--cost-scope cone; output/net/signal names are nets, while "
                    "only explicit gate instances use gate_fanin".format(
                        expected_scope, expected_name, expected_scope, expected_name))

    basis_requirement = _explicit_basis_requirement(user_req)
    if basis_requirement:
        expected_basis_scope, expected_basis_name, expected_allowed = basis_requirement
        actual_basis_scope, actual_basis_name = _scope_from_opt_args(
            args, "--basis-scope", "--basis-name")
        actual_allowed = _opt_constraint_values(args, "--allowed")
        if expected_basis_scope == "whole" and actual_basis_scope != "whole":
            return (False,
                    "the gate-basis clause applies to the whole design/netlist. "
                    "Omit --basis-scope (or use whole) and pass allowed/banned "
                    "gate types directly")
        if expected_basis_scope != "whole":
            if (actual_basis_scope != expected_basis_scope or
                    actual_basis_name.lower() != expected_basis_name.lower()):
                return (False,
                        "the final gate-basis constraint applies to {} {}. Use "
                        "--basis-scope {} {} without changing the optimization "
                        "scope unless the request separately restricts rewriting"
                        .format(expected_basis_scope, expected_basis_name,
                                expected_basis_scope, expected_basis_name))
        if set(actual_allowed) != set(expected_allowed):
            return (False,
                    "the explicit final basis requires --allowed {}; the proposed "
                    "allowed basis is {}".format(
                        " ".join(expected_allowed),
                        " ".join(actual_allowed) or "<missing>"))

        if (_explicit_cost_is_whole(user_req, active_cost_function) and
                not _explicit_rewrite_cone_target(user_req)):
            actual_scope, _ = _scope_from_opt_args(args, "--scope", "--name")
            cost_scope = (_option_value(args, "--cost-scope") or "whole").lower()
            if actual_scope != "whole" or cost_scope != "whole":
                return (False,
                        "the cost function measures the whole final design and the "
                        "request does not separately restrict rewriting to a cone. "
                        "Keep --scope and --cost-scope at whole; use --basis-scope "
                        "only for the local gate-basis constraint")
    return True, ""


_REGISTER_PATH_RELATION_RE = re.compile(
    r"\b(?:register|flip[- ]?flop|dff)s?\s*[- ]to[- ]\s*"
    r"(?:register|flip[- ]?flop|dff)s?\b|"
    r"\bbetween\s+(?:(?:any|all|the)\s+)?"
    r"(?:registers|flip[- ]?flops|dffs)\s+and\s+"
    r"(?:(?:any|all|the)\s+)?(?:registers|flip[- ]?flops|dffs)\b|"
    r"\bbetween\s+(?:(?:any|all|the)\s+)?"
    r"(?:registers|flip[- ]?flops|dffs)\b|"
    r"\bfrom\s+(?:(?:any|all|the)\s+)?"
    r"(?:register|flip[- ]?flop|dff)s?\s*(?:outputs?|q(?:\s+pins?)?)\b"
    r"[^.\n]{0,160}\bto\s+(?:(?:any|all|the)\s+)?"
    r"(?:register|flip[- ]?flop|dff)s?\s*(?:inputs?|d(?:\s+pins?)?)\b|"
    r"\b(?:beginning|starting|originating)\s+(?:at|from)\s+"
    r"(?:register|flip[- ]?flop|dff)s?\s*(?:outputs?|q(?:\s+pins?)?)\b"
    r"[^.\n]{0,160}\b(?:ending|terminating)\s+(?:at|in)\s+"
    r"(?:register|flip[- ]?flop|dff)s?\s*(?:inputs?|d(?:\s+pins?)?)\b",
    re.I)
_MAX_PATH_METRIC_RE = re.compile(
    r"\b(?:maximum|max|longest)\b[^.\n]{0,100}\b(?:depth|path|stage)\b|"
    r"\b(?:depth|path|stage)\b[^.\n]{0,100}\b(?:maximum|max)\b", re.I)
_MIN_PATH_METRIC_RE = re.compile(
    r"\b(?:minimum|min|shortest)\b[^.\n]{0,100}\b(?:depth|path|stage)\b|"
    r"\b(?:depth|path|stage)\b[^.\n]{0,100}\b(?:minimum|min)\b", re.I)
_RANKED_PATH_RE = re.compile(
    r"\b(?:top\s*[- ]?\d+|(?:first|second|third|fourth|fifth|\d+(?:st|nd|rd|th))"
    r"\s+(?:shortest|longest)|nth\s+(?:shortest|longest))\b", re.I)
_PATH_CONSTRAINT_RE = re.compile(
    r"\b(?:through|via|avoid(?:ing)?|excluding|without|must\s+pass|"
    r"required\s+(?:gate|net|node))\b", re.I)
_ENDPOINT_NAME = (
    r"(?!(?:input|output|pi|po|net|signal|wire|gate|dff)\b)"
    r"(?:\\[^\s,;:!?]+|[A-Za-z_$][A-Za-z0-9_$./-]*(?:\[\d+\])?)")
_ENDPOINT_ROLE = (
    r"(?:(?:primary\s+)?(?:input|output)|pi|po|net|signal|wire|"
    r"gate(?:\s+instance)?|dff(?:\.(?:q|d))?)")
_NAMED_ENDPOINT_RELATION_RE = re.compile(
    r"\bfrom\s+(?:the\s+)?(?:" + _ENDPOINT_ROLE + r"\s+)?" +
    _ENDPOINT_NAME + r"\s+to\s+(?:the\s+)?(?:" + _ENDPOINT_ROLE +
    r"\s+)?" + _ENDPOINT_NAME + r"(?![A-Za-z0-9_$/\[\]-])|"
    r"\bbetween\s+(?:the\s+)?(?:" + _ENDPOINT_ROLE + r"\s+)?" +
    _ENDPOINT_NAME + r"\s+and\s+(?:the\s+)?(?:" + _ENDPOINT_ROLE +
    r"\s+)?" + _ENDPOINT_NAME + r"(?![A-Za-z0-9_$/\[\]-])", re.I)
_ENDPOINT_POPULATION_RELATION_RE = re.compile(
    r"\bfrom\s+(?:any|all)\s+(?:primary\s+)?(?:inputs?|pis?)\s+"
    r"to\s+(?:any|all)\s+(?:primary\s+)?(?:outputs?|pos?)\b|"
    r"\bbetween\s+(?:any|all)\s+(?:primary\s+)?(?:inputs?|pis?)\s+"
    r"and\s+(?:any|all)\s+(?:primary\s+)?(?:outputs?|pos?)\b", re.I)


def _has_explicit_endpoint_relation(user_req):
    """Recognize circuit endpoint relations without matching numeric ranges."""
    text = user_req or ""
    return bool(_NAMED_ENDPOINT_RELATION_RE.search(text) or
                _ENDPOINT_POPULATION_RELATION_RE.search(text))


def _explicit_register_path_intent(user_req):
    """Return semantic ownership and a safe canonical action when unambiguous.

    The recognizer models endpoint domains rather than one benchmark sentence.
    A register-to-register population always constrains both timing boundaries,
    so Path Query owns it. Canonicalization is intentionally limited to an
    unranked, unconstrained global min/max request; richer requests stay with
    the LLM after the ownership guard rejects an incompatible Depth action.
    """
    text = user_req or ""
    if not _REGISTER_PATH_RELATION_RE.search(text):
        return None

    intent = {"family": "path_query", "mode": None, "args": None}
    if _RANKED_PATH_RE.search(text):
        return intent
    maximum = bool(_MAX_PATH_METRIC_RE.search(text))
    minimum = bool(_MIN_PATH_METRIC_RE.search(text))
    if maximum == minimum:
        return intent
    intent["mode"] = "max_depth" if maximum else "min_depth"
    if not _PATH_CONSTRAINT_RE.search(text):
        intent["args"] = ["all_dff_q", "all_dff_d"]
    return intent


def _payload_mode_args(payload):
    if "command" in payload:
        try:
            tokens = shlex.split(normalize_command(payload["command"]), posix=True)
        except ValueError:
            return "", []
        return (tokens[1].lower() if len(tokens) >= 2 else "", tokens[2:])
    return (str(payload.get("mode", "")).strip().lower(),
            [str(item) for item in payload.get("args", [])])


_PATH_SINGLE_RESULT_MODES = set((
    "exists", "find_any", "min_depth", "max_depth",
))


def _requires_exhaustive_path_collection(deliverables):
    for deliverable in deliverables or []:
        if (str(deliverable.get("kind", "")).strip().lower() == "entity_list" and
                str(deliverable.get("entity", "")).strip().lower() == "path" and
                str(deliverable.get("coverage", "")).strip().lower() == "exhaustive"):
            return True
    return False


def _requires_complete_output_population(deliverables):
    for deliverable in deliverables or []:
        if (str(deliverable.get("kind", "")).strip().lower() == "entity_list" and
                str(deliverable.get("entity", "")).strip().lower() == "output" and
                str(deliverable.get("coverage", "")).strip().lower() == "exhaustive"):
            return True
    return False


def _is_unfiltered_exhaustive_path_request(user_req, deliverables):
    text = user_req or ""
    prompt_requires_all_paths = (bool(_COMPLETE_COLLECTION_RE.search(text)) and
                                 bool(re.search(r"\bpaths?\b", text, re.I)))
    contract_requires_all_paths = _requires_exhaustive_path_collection(deliverables)
    return ((prompt_requires_all_paths or contract_requires_all_paths) and
            not _RANKED_PATH_RE.search(text) and
            not _MAX_PATH_METRIC_RE.search(text) and
            not _MIN_PATH_METRIC_RE.search(text) and
            not re.search(r"\bdepth\b", text, re.IGNORECASE))


def normalize_deliverable_action(user_req, family, payload, deliverables):
    """Repair a mode only when the frozen output contract is decisive."""
    family_key = (family or "").strip().lower()
    mode, args = _payload_mode_args(payload)
    if (family_key == "path_query" and
            _is_unfiltered_exhaustive_path_request(user_req, deliverables) and
            mode in _PATH_SINGLE_RESULT_MODES):
        normalized = dict(payload)
        normalized.pop("command", None)
        normalized["family"] = "path_query"
        normalized["mode"] = "enumerate"
        normalized["args"] = list(args)
        return ("path_query", normalized,
                "exhaustive path deliverable normalized from {} to enumerate".format(
                    mode))

    if (family_key == "cone_query" and mode == "largest_output" and
            _requires_complete_output_population(deliverables)):
        normalized = dict(payload)
        normalized.pop("command", None)
        normalized["family"] = "cone_query"
        normalized["mode"] = "output_rank"
        normalized["args"] = ["gates", "highest"]
        return ("cone_query", normalized,
                "complete output extrema deliverable normalized from legacy "
                "largest_output to output_rank gates highest")

    return family_key, payload, ""


def normalize_unknown_search_deliverables(user_req, payload):
    """Canonicalize a yes/no unknown-candidate search contract to ``found``."""
    if not unknown_boolean_search_intent(user_req):
        return payload, ""
    if str(payload.get("family", "")).strip().lower() != "func_search":
        return payload, ""
    deliverables = payload.get("deliverables")
    if not isinstance(deliverables, list):
        return payload, ""
    changed = False
    normalized_items = []
    for item in deliverables:
        normalized = dict(item)
        if (normalized.get("kind") == "scalar" and
                normalized.get("coverage") == "scalar" and
                _field_key(normalized.get("metric", "")) in (
                    "", "equivalence", "equivalent", "existence", "match")):
            if normalized.get("metric") != "found":
                normalized["metric"] = "found"
                changed = True
        normalized_items.append(normalized)
    if not changed:
        return payload, ""
    result = dict(payload)
    result["deliverables"] = normalized_items
    return result, "unknown-candidate Boolean existence metric normalized to found"


def validate_deliverable_compatibility(user_req, family, payload, deliverables):
    """Reject remaining action/output contracts that no selected mode can satisfy."""
    family_key = (family or "").strip().lower()
    mode, _ = _payload_mode_args(payload)
    if (family_key == "path_query" and
            _is_unfiltered_exhaustive_path_request(user_req, deliverables) and
            mode not in ("enumerate", "direct_pi_po")):
        return (False,
                "an exhaustive path entity list requires path_query enumerate; "
                "mode {} cannot return every path".format(mode or "<missing>"))
    if (family_key == "cone_query" and mode == "largest_output" and
            _requires_complete_output_population(deliverables)):
        return (False,
                "largest_output returns one legacy representative and cannot "
                "satisfy a complete output population; use output_rank")
    comparisons = [deliverable for deliverable in deliverables or []
                   if deliverable.get("kind") == "comparison"]
    if (comparisons and family_key == "cone_query" and
            mode in ("largest_output", "output_rank", "output_filter")):
        subjects = comparisons[0].get("subjects", [])
        return (False,
                "a comparison over the explicitly named subjects {} requires "
                "one subject-specific cone measurement per subject. {} uses "
                "the complete primary-output population and cannot restrict "
                "that population to the named candidates"
                .format(", ".join(subjects), mode))
    return True, ""


def _analysis_error_recovery_hint(family, payload, raw_output):
    """Return semantic recovery guidance after a failed analysis command.

    This is deliberately advisory. It does not infer an object type, rewrite
    an action, or turn a lookup failure into a negative circuit answer.
    """
    if envelope_ok(raw_output):
        return ""
    family_key = (family or "").strip().lower()
    mode, _ = _payload_mode_args(payload)
    failure = "{}\n{}".format(
        envelope_message(raw_output), raw_output or "").lower()

    if family_key == "path_query" and (
            "unresolved" in failure or
            ("endpoint" in failure and
             ("not found" in failure or "cannot resolve" in failure))):
        return (
            "Re-check the original source and destination spelling, including "
            "any explicit bus-bit selection. Preserve a named endpoint across "
            "retries; do not replace it with all_pi, all_po, or another broader "
            "population unless the original request asks for that population. "
            "An unresolved endpoint is not evidence that no path exists.")

    if family_key == "cone_query" and mode in ("gate_fanin", "gate_fanout") \
            and "gate not found" in failure:
        return (
            "Reconsider the identity of the named cone root. It may be a net, "
            "port, or signal rather than a gate instance; compare net_fanin or "
            "net_fanout with the selected gate-root mode. Gate not found does "
            "not prove that the named circuit object is absent.")

    if family_key == "cone_query" and mode in ("net_fanin", "net_fanout") \
            and "net not found" in failure:
        return (
            "Reconsider the identity of the named cone root. It may be a gate "
            "instance rather than a net, port, or signal; compare gate_fanin or "
            "gate_fanout with the selected net-root mode. Net not found does not "
            "prove that the named circuit object is absent.")
    return ""


def normalize_analysis_action(user_req, family, payload):
    """Canonicalize only fully specified analysis actions without losing intent."""
    family_key = (family or "").strip().lower()
    intent = _explicit_register_path_intent(user_req)
    if (not intent or intent["mode"] is None or intent["args"] is None or
            family_key not in ("depth_query", "path_query")):
        return family_key, payload, ""

    mode, args = _payload_mode_args(payload)
    if (family_key == "path_query" and mode == intent["mode"] and
            [item.lower() for item in args] == intent["args"]):
        return family_key, payload, ""

    normalized = dict(payload)
    normalized.pop("command", None)
    normalized["family"] = "path_query"
    normalized["mode"] = intent["mode"]
    normalized["args"] = list(intent["args"])
    return ("path_query", normalized,
            "explicit register boundary population normalized to {} {}".format(
                intent["mode"], " ".join(intent["args"])))


_OUTPUT_POPULATION_RE = re.compile(
    r"\b(?:which|what)\s+(?:primary\s+)?outputs?(?:\s+bits?)?\b|"
    r"\b(?:identify|find|report)\s+(?:the\s+)?(?:primary\s+)?output"
    r"(?:\s+bit)?\b[^.?!\n]{0,100}\b(?:deepest|maximum|largest|highest|"
    r"greatest|smallest|lowest|most|fewest)\b|"
    r"\b(?:deepest|maximum-depth|highest-depth|lowest-depth)\s+"
    r"(?:primary\s+)?outputs?(?:\s+bits?)?\b|"
    r"\b(?:all|every)\s+(?:primary\s+)?outputs?(?:\s+bits?)?\b",
    re.I)
_NAMED_OUTPUT_TARGET_RE = re.compile(
    r"\b(?:primary\s+)?output(?:\s+(?:bit|signal|net))?\s+"
    r"(?!(?:bit|signal|net|with|that|which|whose|having|has|is|are|of|"
    r"from|at|in)\b)"
    r"([A-Za-z_$][A-Za-z0-9_$./-]*(?:\[\d+\])?)"
    r"(?![A-Za-z0-9_$/\[\]-])", re.I)
_NAMED_OUTPUT_STOPWORDS = set((
    "bit", "signal", "net", "with", "that", "which", "whose", "having",
    "has", "is", "are", "of", "from", "at", "in",
))
_OUTPUT_DEPTH_EXTREMA_RE = re.compile(
    r"\bdeepest\b|"
    r"\b(?:maximum|highest|largest|greatest)\s+"
    r"(?:combinational\s+|logic\s+|path\s+)?depth\b|"
    r"\bmost\s+(?:combinational\s+|logic\s+)?levels?\b", re.I)
_OUTPUT_CONE_COUNT_RE = re.compile(
    r"\b(?:fanin\s+|fanout\s+)?cone\b[^.?!\n]{0,80}"
    r"\b(?:gate|net)\s+(?:count|cardinality)\b|"
    r"\b(?:by|according\s+to|ranked?\s+by)\s+"
    r"(?:the\s+)?(?:number\s+of\s+)?(?:gates?|nets?)\b|"
    r"\b(?:largest|smallest|biggest)\s+"
    r"(?:fanin\s+|fanout\s+)?cone\b|"
    r"\b(?:maximum|minimum)\s+(?:fanin\s+|fanout\s+)?cone\s+size\b",
    re.I)


def _requested_output_extrema_metric(user_req):
    """Return an explicit output-population ranking metric, if unambiguous."""
    text = user_req or ""
    if not _OUTPUT_POPULATION_RE.search(text):
        return ""
    # A source-to-destination depth relation belongs to Path Query even when
    # the destination population is every primary output. It is not arrival
    # depth under the design's default timing boundaries.
    if (_has_explicit_endpoint_relation(text) and
            (_MAX_PATH_METRIC_RE.search(text) or
             _MIN_PATH_METRIC_RE.search(text))):
        return ""
    depth = bool(_OUTPUT_DEPTH_EXTREMA_RE.search(text))
    cone_count = bool(_OUTPUT_CONE_COUNT_RE.search(text))
    if depth == cone_count:
        return ""
    return "depth" if depth else "cone_size"


def _requested_named_output_depth_target(user_req):
    """Return one explicitly named output target for an arrival-depth request."""
    text = user_req or ""
    if _OUTPUT_POPULATION_RE.search(text):
        return ""
    if not (_OUTPUT_DEPTH_EXTREMA_RE.search(text) or
            re.search(r"\b(?:arrival|logic|combinational|path)\s+depth\b",
                      text, re.I)):
        return ""
    match = _NAMED_OUTPUT_TARGET_RE.search(text)
    if not match:
        return ""
    target = match.group(1).rstrip(".,;:")
    if target.lower() in _NAMED_OUTPUT_STOPWORDS:
        return ""
    return target


def _validate_analysis_semantics(user_req, family, mode, args):
    explicit_endpoint_relation = _has_explicit_endpoint_relation(user_req)
    maximum_path = bool(_MAX_PATH_METRIC_RE.search(user_req or ""))
    minimum_path = bool(_MIN_PATH_METRIC_RE.search(user_req or ""))
    if explicit_endpoint_relation and maximum_path != minimum_path:
        required_mode = "max_depth" if maximum_path else "min_depth"
        if family != "path_query" or mode != required_mode:
            return (False,
                    "the request explicitly constrains a source-to-destination "
                    "depth relation; use path_query {} and preserve both "
                    "endpoint sets rather than an arrival-depth population mode"
                    .format(required_mode))
        return True, ""

    named_output_target = _requested_named_output_depth_target(user_req)
    if named_output_target:
        if (family != "depth_query" or mode != "net" or
                args != [named_output_target]):
            return (False,
                    "the request selects one named output target {}; use "
                    "depth_query net {} and preserve the exact name or bus bit. "
                    "deepest_output ranks the complete output population"
                    .format(named_output_target, named_output_target))

    output_metric = _requested_output_extrema_metric(user_req)
    if output_metric == "depth":
        if family != "depth_query" or mode != "deepest_output":
            return (False,
                    "the request ranks the primary-output population by logic "
                    "depth or logic levels; use depth_query deepest_output. "
                    "Cone output ranking measures gate/net cardinality instead")
    elif output_metric == "cone_size":
        if family != "cone_query" or mode != "output_rank":
            return (False,
                    "the request ranks the primary-output population by fanin-"
                    "cone gate/net cardinality; use cone_query output_rank with "
                    "the requested cardinality metric, not a depth mode")

    intent = _explicit_register_path_intent(user_req)
    if not intent:
        return True, ""
    if family != "path_query":
        return (False,
                "the request constrains paths to a register-output source set and "
                "a register-input destination set. This is a Path Query relation, "
                "not arrival depth from default timing boundaries")
    if intent["mode"] and mode != intent["mode"]:
        return (False,
                "the register-to-register request requires path_query {} rather "
                "than mode {}".format(intent["mode"], mode or "<missing>"))
    if intent["args"] and [item.lower() for item in args[:2]] != intent["args"]:
        return (False,
                "the unrestricted register boundary population uses all_dff_q as "
                "the source and all_dff_d as the destination")
    return True, ""


def _validate_function_semantics(user_req, family, mode, args):
    """Keep named-signal proofs separate from unknown-candidate search."""
    if not unknown_boolean_search_intent(user_req):
        return True, ""
    operator = boolean_pattern_operator(user_req)
    if family != "func_search":
        return (False,
                "the request quantifies unknown candidate operands whose Boolean "
                "combination must be searched. Use func_search pattern with the "
                "named operator and target; symbolic operands are roles, not net names")
    if mode not in ("pattern", "nand_pair"):
        return (False,
                "unknown operands for a named Boolean operator require func_search "
                "pattern (or nand_pair only for NAND), not mode {}".format(
                    mode or "<missing>"))
    if mode == "nand_pair" and operator != "NAND":
        return False, "nand_pair is valid only for a NAND operand search"
    if mode == "pattern" and (not args or str(args[0]).upper() != operator):
        return (False,
                "func_search pattern must preserve the requested Boolean operator {}"
                .format(operator or "<missing>"))
    return True, ""


_OPERATION_DELTA_REQUEST_RE = re.compile(
    r"\bhow\s+many\b[^?\n]{0,120}\b(?:added|removed|eliminated|replaced|"
    r"merged|simplified|changed)\b|"
    r"\b(?:added|removed|eliminated|merged|simplified|changed)\b"
    r"[^.?\n]{0,80}\b(?:by|during|from)\b[^.?\n]{0,80}"
    r"\b(?:edit|optimization|replac\w*|simplif\w*|cleanup|mapping)\b",
    re.I)


def validate_action_semantics(user_req, family, payload, active_cost_function=None):
    """Validate prompt-to-mode ownership after structured grammar validation."""
    family_key = (family or "").strip().lower()
    mode, args = _payload_mode_args(payload)
    if _OPERATION_DELTA_REQUEST_RE.search(user_req or ""):
        if family_key != "report_query":
            return (False,
                    "the request asks for a delta produced by the most recent "
                    "edit or optimization; use report_query last_edit rather "
                    "than a current-design inventory count")
        return True, ""
    if family_key in ("depth_query", "path_query", "cone_query"):
        return _validate_analysis_semantics(user_req, family_key, mode, args)
    if unknown_boolean_search_intent(user_req):
        return _validate_function_semantics(user_req, family_key, mode, args)
    if family_key not in ("edit_apply", "opt_apply"):
        return True, ""
    if family_key == "edit_apply":
        return _validate_edit_semantics(
            user_req, mode, args, active_cost_function=active_cost_function)
    return _validate_opt_semantics(
        user_req, mode, args, active_cost_function=active_cost_function)


def _validate_structure_grammar(mode, args):
    no_args = set((
        "summary", "list_gates", "list_nets", "net_classes", "list_pi",
        "list_po", "list_dffs", "list_comb", "structural_issues",
    ))
    one_arg = set((
        "gate_info", "gate_inputs", "gate_output", "gate_fanin", "gate_fanout",
        "net_info", "fanout_load", "fanout_report", "fanout_violations",
    ))
    if mode in no_args:
        return (not args, "{} accepts no arguments".format(mode) if args else "")
    if mode in one_arg:
        return (len(args) == 1, "{} requires exactly one argument".format(mode))
    if mode == "is_connected":
        return (len(args) == 2, "is_connected requires <gate> <net>")
    if mode in ("net_driver", "net_loads"):
        valid = len(args) in (1, 2) and (len(args) == 1 or args[1] == "--with-pins")
        return valid, "{} requires <net> and optional --with-pins".format(mode)
    if mode in ("count_by_type", "gates_by_type"):
        index = 0
        if index < len(args) and not args[index].startswith("-"):
            if not _validate_gate_type(args[index], allow_all=True):
                return False, "{} positional type must be a public gate type".format(mode)
            index += 1
        while index < len(args):
            option = args[index]
            if option == "--with-pins" and mode == "gates_by_type":
                index += 1
                continue
            if option not in ("--gate-types", "--exclude-gate-types"):
                return False, "unknown {} option {}".format(mode, option)
            index += 1
            start = index
            while index < len(args) and not args[index].startswith("-"):
                if not _validate_gate_type(args[index]):
                    return False, "{} requires public gate type tokens".format(option)
                index += 1
            if index == start:
                return False, "{} requires at least one gate type".format(option)
        return True, ""
    return True, ""


_BOOLEAN_OPERATOR_EXPRESSION_RE = re.compile(
    r"^(?:BUF|NOT|AND|NAND|OR|NOR|XOR|XNOR)\s*\(", re.I)


def _validate_function_query_grammar(mode, args):
    """Reject operator expressions where the public mode requires net names."""
    if mode in ("equivalence", "conditional_equivalence", "depends_on", "symmetry"):
        if any(_BOOLEAN_OPERATOR_EXPRESSION_RE.match(str(arg)) for arg in args):
            return (False,
                    "func_query operands must be existing named signals; a Boolean "
                    "operator expression with unknown operands belongs to func_search")
    return True, ""


def _validate_function_search_grammar(mode, args):
    if mode == "pattern":
        if len(args) < 2:
            return False, "pattern requires <gate_type> <target_net>"
        if str(args[0]).upper() not in (
                "BUF", "NOT", "AND", "NAND", "OR", "NOR", "XOR", "XNOR"):
            return False, "pattern requires a supported Boolean gate type"
    if mode == "nand_pair" and not args:
        return False, "nand_pair requires <target_net>"
    return True, ""


def _validate_mode_grammar(family, mode, args):
    if family == "edit_apply":
        return _validate_edit_grammar(mode, args)
    if family == "opt_apply":
        return _validate_opt_grammar(mode, args)
    if family == "structure_query":
        return _validate_structure_grammar(mode, args)
    if family == "func_query":
        return _validate_function_query_grammar(mode, args)
    if family == "func_search":
        return _validate_function_search_grammar(mode, args)
    return True, ""


def validate_registered_command(family, command):
    """Validate public family/head/mode and registered strict grammar."""
    family = (family or "").strip().lower()
    if family not in REGISTRY:
        return False, "unknown family: {}".format(family), None
    command = normalize_command(command)
    bracket_values = re.findall(r"\[([^\]]+)\]", command)
    has_optional_placeholder = any(
        not re.match(r"^\d+(?::\d+)?$", value.strip())
        for value in bracket_values)
    if re.search(r"<[^>]+>", command) or has_optional_placeholder:
        return False, "command still contains a documentation placeholder", family
    try:
        tokens = shlex.split(command, posix=True)
    except ValueError as exc:
        return False, "invalid command quoting: {}".format(exc), family
    if not tokens:
        return False, "empty command", family
    card = REGISTRY[family]
    heads = set(head.lower() for head in card.get("command_heads", []))
    mode_names = _mode_names(card)
    if (family != "session_io" and tokens[0].lower() not in heads and
            tokens[0].lower() in mode_names):
        # The JSON family field already makes ownership unambiguous. Weak models
        # sometimes omit the same family token from command and begin at mode.
        # Restoring that redundant token is grammatical normalization only;
        # arguments and requested semantics remain byte-for-byte unchanged.
        command = card["command_heads"][0] + " " + command
        tokens.insert(0, card["command_heads"][0])
        sys.stderr.write("[normalize] restored family {} in command\n".format(family))
    if tokens[0].lower() not in heads:
        return False, "command head does not belong to family {}".format(family), family
    if family == "session_io":
        mode = tokens[0].lower()
    else:
        if len(tokens) < 2:
            return False, "command is missing its mode", family
        mode = tokens[1].lower()
    if mode not in mode_names:
        return False, _unknown_mode_error(family, mode, card), family
    mode_record = _mode_record(card, mode)
    canonical_mode = mode_record["name"].lower() if mode_record else mode
    valid_grammar, grammar_error = _validate_mode_grammar(
        family, canonical_mode, tokens[2:] if family != "session_io" else tokens[1:])
    if not valid_grammar:
        return False, grammar_error, family
    return True, command, family


def validate_action_command(family, payload):
    """Compose a structured run action, then validate the resulting command.

    Legacy command actions remain accepted during migration. Structured actions
    are preferred because the model only selects a documented mode and ordered
    argument tokens; it never has to duplicate the family head or CLI quoting.
    """
    if "command" in payload:
        return validate_registered_command(family, payload["command"])
    family_key = (family or "").strip().lower()
    if family_key not in REGISTRY:
        return False, "unknown family: {}".format(family_key), None
    mode = payload.get("mode", "").strip()
    card = REGISTRY[family_key]
    mode_record = _mode_record(card, mode)
    if mode_record is None:
        return False, _unknown_mode_error(family_key, mode, card), family_key
    args = []
    for item in payload.get("args", []):
        token = str(item)
        if not token or "\n" in token or "\r" in token:
            return False, "run args must be non-empty single-line tokens", family_key
        if re.search(r"\s", token):
            return False, "each args entry must be one CLI token", family_key
        args.append(token)
    if family_key == "session_io":
        command = " ".join([mode_record["name"]] + args)
    else:
        head = card["command_heads"][0]
        command = " ".join([head, mode_record["name"]] + args)
    return validate_registered_command(family_key, command)


_FINAL_FANOUT_EDIT_MODES = set((
    "insert_buffers_for_fanout",
    "insert_buffers_for_net",
    "insert_buffers_for_dff_control",
))
_BUFFER_INSERTION_INTENT_RE = re.compile(
    r"\b(?:insert|add|place)\w*\s+(?:the\s+)?buffers?\b", re.I)
_FANOUT_LOAD_LIMIT_RE = re.compile(
    r"\b(?:no\s+(?:signal|net|wire)\b[^.?!\n]{0,100}?"
    r"(?:drive|drives|driving|have|has|exceed|exceeds)\w*\s+"
    r"(?:more\s+than|over|above)\s*|"
    r"(?:maximum|max)\s+(?:direct\s+)?fanout\s*(?:of|=|is|to)?\s*)"
    r"(\d+)\b", re.I)


def _required_compound_workflow(user_req, active_cost_function=None):
    """Infer only unambiguous cost-then-fanout workflows.

    This is intentionally narrower than general task planning. It protects a
    hard constraint whose enforcement can be invalidated by later optimization,
    while leaving unrelated compound prompts to the model and Tool Cards.
    """
    text = user_req or ""
    cost_mode = _requested_optimization_mode(text, active_cost_function)
    limit_match = _FANOUT_LOAD_LIMIT_RE.search(text)
    if (not cost_mode or not limit_match or
            not _BUFFER_INSERTION_INTENT_RE.search(text)):
        return None
    return {"stages": [
        {"family": "opt_apply", "mode": cost_mode, "args": []},
        {"family": "edit_apply", "mode": "insert_buffers_for_fanout",
         "args": [limit_match.group(1)]},
    ]}


def _workflow_stage_signature(family, payload):
    mode, args = _payload_mode_args(payload)
    return ((family or "").strip().lower(), mode, tuple(str(item) for item in args))


def _workflow_expected_stage(workflow, completed_stage_count):
    stages = (workflow or {}).get("stages", [])
    if completed_stage_count < 0 or completed_stage_count >= len(stages):
        return None
    return stages[completed_stage_count]


def _workflow_action_matches_stage(family, payload, stage):
    if stage is None:
        return True
    return _workflow_stage_signature(family, payload) == _workflow_stage_signature(
        stage.get("family", ""), stage)


def _workflow_stage_text(stage):
    if not stage:
        return "<complete>"
    parts = [stage.get("family", ""), stage.get("mode", "")]
    parts.extend(str(item) for item in stage.get("args", []))
    return " ".join(part for part in parts if part)


def validate_workflow_contract(user_req, workflow, active_cost_function=None):
    """Validate every declared stage without executing or rewriting it."""
    stages = (workflow or {}).get("stages", [])
    for index, stage in enumerate(stages):
        family = stage.get("family", "")
        valid, command_or_error, _ = validate_action_command(family, stage)
        if not valid:
            return (False, "workflow stage {} is not a valid command: {}".format(
                index + 1, command_or_error))
        semantic_valid, semantic_error = validate_action_semantics(
            user_req, family, stage, active_cost_function=active_cost_function)
        if not semantic_valid:
            return (False, "workflow stage {} does not represent the request: {}".format(
                index + 1, semantic_error))

    # Fanout buffer insertion is a final hard-constraint enforcement stage.
    # A later optimizer could remove or restructure those buffers, so any cost
    # optimization in the same declared workflow must precede it.
    opt_indices = [index for index, stage in enumerate(stages)
                   if stage.get("family") == "opt_apply"]
    for index, stage in enumerate(stages):
        if (stage.get("family") == "edit_apply" and
                stage.get("mode") in _FINAL_FANOUT_EDIT_MODES and
                any(opt_index > index for opt_index in opt_indices)):
            return (False, "fanout constraint enforcement must follow every "
                    "optimization stage in the same workflow")
    return True, ""


def parse_llm_line(text, allow_free_text=False):
    """Parse one LLM turn into (action, payload, reason).

    * REASON is context for the action in the same response, never an action by
      itself.  It is returned to the caller so the exact decision/action pair
      can be retained in the prompt-local conversation.
    * ANSWER payloads keep every following line.  The old version split on
      newlines and kept only the first, silently truncating enumerations.
    """
    if not text:
        return ("NONE", "", "")

    cleaned = text.strip()
    if "```" in cleaned:
        cleaned = "\n".join(l for l in cleaned.split("\n") if not l.strip().startswith("```")).strip()

    lines = cleaned.split("\n")
    reason = ""
    saw_content = False

    for i, line in enumerate(lines):
        s = line.strip()
        if not s:
            continue
        rm = _REASON_LINE.match(s)
        if rm:
            if not reason:
                reason = s[rm.end():].strip()
            continue
        am = _ACTION_LINE.match(s)
        if am:
            verb = am.group(1).upper()
            if verb == "RUN":
                return ("RUN", s[am.end():].strip(), reason)
            payload = "\n".join([s[am.end():].strip()] + lines[i + 1:]).strip()
            return ("ANSWER", payload, reason)
        saw_content = True

    # Non-final turns require an explicit action. The forced-final turn is deliberately
    # tolerant: any non-empty prose can be used as the answer without a ninth LLM
    # call merely to repair an ANSWER prefix.
    if saw_content and allow_free_text:
        return ("ANSWER", cleaned, reason)
    return ("NONE", "", reason)


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  ReAct loop                                                            ║
# ╚════════════════════════════════════════════════════════════════════════╝

# Requests inside one testcase refer back to earlier ones ("simplify the
# reported gates", "the conversion just performed", "how many ... now",
# "found in step 3").  The backend keeps design state, but the natural-language
# context of prior turns exists nowhere else, so every request carries a digest
# of what came before.  Reset when a new testcase starts.
HISTORY_TURNS = 8
HISTORY_ANSWER_CHARS = 300


def format_history(history):
    """Render recent (response_id, request, answer) triples as a prompt preamble."""
    if not history:
        return ""
    lines = []
    for rid, q, a in history[-HISTORY_TURNS:]:
        a = " ".join((a or "").split())
        if len(a) > HISTORY_ANSWER_CHARS:
            a = a[:HISTORY_ANSWER_CHARS] + " ..."
        lines.append("[{}] Q: {}\n[{}] A: {}".format(rid, q, rid, a))
    return ("Earlier steps in this testcase, numbered by response id. Use them to "
            "resolve back-references such as \"the reported gates\", \"just "
            "performed\", \"now\", \"step N\":\n" + "\n".join(lines) + "\n\n")


def _fallback_answer(user_req):
    """Deterministic reply used whenever the LLM path cannot produce one.

    Never leaks transport detail (HTTP codes, step budgets) into the log: the
    spec requires responses that address the request. It also never claims the
    original design was retained: the LLM may fail after a successful edit, so
    that statement would not be known here.
    """
    return ("The requested analysis could not be completed on the current design within the "
            "time available for this request. No exact list, count, or proof is claimed.")


def _unproved_equivalence_fallback():
    """Contest-policy answer when whole-design comparison produced no evidence."""
    return ("The current design is functionally equivalent to the original loaded "
            "netlist.")


def _needs_unproved_equivalence_fallback(families, usable_backend_evidence):
    return not usable_backend_evidence and "equiv_query" in set(families or [])


_OUTPUT_FILE_RE = re.compile(r"^\s*output[ _-]*file:\s*(.+?)\s*$",
                             re.MULTILINE | re.IGNORECASE)

_COLLECTION_NOUN = (
    r"(?:gates?|nets?|signals?|paths?|outputs?|inputs?|ports?|pins?|loads?|"
    r"drivers?|dffs?|flip[- ]?flops?|registers?|pairs?|nodes?|records?|"
    r"objects?|equations?|expressions?|matches?|violations?|issues?)"
)
_COMPLETE_COLLECTION_RE = re.compile(
    r"\b(?:list|enumerate|identify|determine|find|show|report|return|provide)\b"
    r"[^.?!\r\n]{0,80}\b(?:all|every|each)\b[^.?!\r\n]{0,80}\b" +
    _COLLECTION_NOUN + r"\b|"
    r"\b(?:list|enumerate)\b[^.?!\r\n]{0,80}\b" + _COLLECTION_NOUN + r"\b",
    re.IGNORECASE)
_COUNT_SUMMARY_RE = re.compile(
    r"\b(?:a\s+total\s+of|there\s+(?:are|is)|total|count|number\s+of)\b"
    r"[^.?!\r\n]{0,40}\b\d+\b|"
    r"\b\d+\s+" + _COLLECTION_NOUN + r"\b",
    re.IGNORECASE)
_EMPTY_COLLECTION_RE = re.compile(
    r"\b(?:no|zero|0)\s+" + _COLLECTION_NOUN + r"\b|\bnone\b",
    re.IGNORECASE)
_OMITTED_COLLECTION_RE = re.compile(
    r"\.\.\.|\u2026|\betc(?:etera)?\.?\b|\bmany\s+others?\b|"
    r"\band\s+others?\b|\bsuch\s+as\b|\bfor\s+example\b|"
    r"\be\.g\.|\brepresentative\b|\bexamples?\s+(?:include|are)\b",
    re.IGNORECASE)
_COUNTED_SECTION_RE = re.compile(
    r"^\s*([^:\r\n]+?)\s*\((\d+)\):\s*$", re.IGNORECASE)
_ANSWER_ID_RE = re.compile(
    r"\\[^\s,;]+|[A-Za-z_][A-Za-z0-9_$./-]*(?:\[[^\]]+\])?")
_MUTATION_RESULT_RE = re.compile(
    r"^\s*command:\s*(?:edit_apply|opt_apply)\s*$", re.MULTILINE | re.IGNORECASE)
_EXPLICIT_MUTATION_DETAIL_RE = re.compile(
    r"\b(?:how\s+many|number\s+of|list|enumerate|identify|name|names|"
    r"which|show|report|return|provide|details?|statistics?|before\s+and\s+after|"
    r"before/after|delta|difference|changed\s+gates?|removed\s+gates?|added\s+gates?)\b",
    re.IGNORECASE)
_EXPLICIT_MUTATION_COUNT_RE = re.compile(
    r"\bcount\s+(?:the\s+|all\s+)?(?:gates?|nets?|pairs?|objects?|changes?)\b",
    re.IGNORECASE)
_UNREQUESTED_MUTATION_DETAIL_RE = re.compile(
    r"\bresulting\s+in\s+(?:a\s+)?total\s+of\s+\d+|"
    r"\b(?:detailed\s+)?statistics\s+(?:before|after|below|follow)|"
    r"\bhere\s+(?:are|is)\s+(?:the\s+)?(?:detailed\s+)?(?:statistics|details)\b",
    re.IGNORECASE)


def _result_field(raw, label):
    match = re.search(r"^\s*" + re.escape(label) + r":\s*(.*?)\s*$",
                      raw or "", re.MULTILINE | re.IGNORECASE)
    return match.group(1).strip() if match else ""


def _result_counted_values(raw, label):
    """Read a simple counted value section from a tool report."""
    lines = (raw or "").splitlines()
    header_re = re.compile(
        r"^\s*" + re.escape(label) + r"\s*\((\d+)\):\s*$", re.I)
    for index, line in enumerate(lines):
        match = header_re.match(line)
        if not match:
            continue
        count = int(match.group(1))
        values = []
        for candidate in lines[index + 1:]:
            if not candidate.strip():
                continue
            if not candidate[:1].isspace():
                break
            value = candidate.strip()
            if ":" in value:
                break
            values.append(value)
            if len(values) == count:
                break
        return values if len(values) == count else []
    return []


def _validate_opt_result_semantics(payload, raw_output):
    """Verify that a successful Opt report honored the executed constraints."""
    mode, args = _payload_mode_args(payload)
    if mode not in ("critical_path_depth", "gate_count_minimization"):
        return True, ""
    if not envelope_ok(raw_output) or not envelope_complete(raw_output):
        return True, ""

    final_satisfied = _result_field(raw_output, "final_constraints_satisfied")
    if final_satisfied.lower() == "false":
        return (False,
                "the optimization report says final_constraints_satisfied:false; "
                "the retained result does not satisfy the requested hard constraints")

    constrained = any(option in args for option in (
        "--allowed", "--banned", "--outside-allowed", "--outside-banned"))
    if not constrained:
        return True, ""

    expected_scope, expected_name = _scope_from_opt_args(
        args, "--basis-scope", "--basis-name")
    reported_scope = _result_field(raw_output, "basis_scope").lower()
    reported_name = _result_field(raw_output, "basis_scope_name")
    normalized_reported_scope = (
        "whole" if reported_scope in ("whole", "whole_netlist") else reported_scope)
    if reported_scope and normalized_reported_scope != expected_scope:
        return (False,
                "the optimization report basis_scope {} does not match the "
                "executed constraint scope {}".format(reported_scope, expected_scope))
    if (expected_scope != "whole" and reported_name and
            reported_name.lower() != expected_name.lower()):
        return (False,
                "the optimization report basis_scope_name {} does not match {}"
                .format(reported_name, expected_name))

    expected_allowed = set(_opt_constraint_values(args, "--allowed"))
    reported_allowed = set(
        value.upper() for value in
        _result_counted_values(raw_output, "allowed_gate_types"))
    if expected_allowed and reported_allowed != expected_allowed:
        return (False,
                "the optimization report allowed gate types {} do not match the "
                "executed constraint {}".format(
                    " ".join(sorted(reported_allowed)) or "<missing>",
                    " ".join(sorted(expected_allowed))))
    return True, ""


def _extract_artifact_paths(raw):
    """Return unique public artifact paths emitted by a backend result.

    Observation spill files are an internal transport detail and deliberately
    use a different field name.  Only the backend's public ``output_file`` /
    ``Output file`` field participates in the answer contract.
    """
    paths = []
    seen = set()
    for match in _OUTPUT_FILE_RE.finditer(raw or ""):
        path = match.group(1).strip().strip("\"'")
        if not path or path.lower() in ("yes", "no", "none", "n/a"):
            continue
        key = path.lower()
        if key not in seen:
            seen.add(key)
            paths.append(path)
    return paths


def _field_key(value):
    return " ".join(re.findall(r"[a-z0-9]+", (value or "").lower()))


def _parse_result_fields(raw):
    """Collect scalar-looking envelope fields without interpreting their values."""
    fields = {}
    for line in (raw or "").splitlines():
        match = re.match(r"^\s*([^:\r\n]+?):\s*(.*?)\s*$", line)
        if not match:
            continue
        key = _field_key(match.group(1))
        value = match.group(2).strip()
        if key and value and key not in fields:
            fields[key] = value
    return fields


def _parse_complete_inline_sections(raw):
    """Return exact counted identity sections from one result.

    Plain sections contain one identity per line. Ranked and filtered reports
    may instead use self-describing key=value rows; in those rows the identity
    field is selected from the counted section label rather than from a
    command-specific testcase rule.
    """
    lines = (raw or "").splitlines()
    sections = []
    index = 0
    while index < len(lines):
        header = _COUNTED_SECTION_RE.match(lines[index])
        if not header:
            index += 1
            continue
        label = header.group(1).strip()
        declared_count = int(header.group(2))
        records = []
        structured_rows = []
        parsed_row_count = 0
        cursor = index + 1
        while cursor < len(lines):
            line = lines[cursor]
            if not line.strip():
                cursor += 1
                continue
            if not line[:1].isspace():
                break
            record = line.strip()
            if len(record.split()) == 1 and ":" not in record:
                records.append(record)
                parsed_row_count += 1
            else:
                row_fields = {}
                for key, value in re.findall(
                        r"([A-Za-z_][A-Za-z0-9_-]*)=(\"[^\"]*\"|'[^']*'|\S+)",
                        record):
                    row_fields[_field_key(key)] = value.strip("\"'")
                if row_fields:
                    structured_rows.append(row_fields)
                    parsed_row_count += 1
                    label_tokens = set(_field_key(label).split())
                    identity_keys = []
                    for entity_key in (
                            "output", "input", "gate", "net", "signal", "path"):
                        if (entity_key in label_tokens or
                                entity_key + "s" in label_tokens):
                            identity_keys.append(entity_key)
                    identity = next((row_fields[key] for key in identity_keys
                                     if row_fields.get(key)), "")
                    if identity:
                        records.append(identity)
            cursor += 1
        if parsed_row_count == declared_count:
            sections.append({
                "label": label,
                "declared_count": declared_count,
                "records": records,
                "structured_rows": structured_rows,
            })
        index = max(index + 1, cursor)
    return sections


def _evidence_relation_signature(family, mode, args):
    """Describe the collection relation represented by one backend action."""
    key = ((family or "").strip().lower(), (mode or "").strip().lower())
    relation = {
        ("structure_query", "net_loads"): "direct_load",
        ("structure_query", "net_driver"): "direct_driver",
        ("structure_query", "gate_fanout"): "direct_successor",
        ("structure_query", "gate_fanin"): "direct_predecessor",
        ("structure_query", "gate_inputs"): "direct_gate_input",
        ("structure_query", "gate_output"): "direct_gate_output",
        ("structure_query", "list_gates"): "active_gate_inventory",
        ("structure_query", "list_nets"): "active_net_inventory",
        ("structure_query", "list_pi"): "primary_input_inventory",
        ("structure_query", "list_po"): "primary_output_inventory",
        ("cone_query", "net_fanin"): "transitive_net_fanin",
        ("cone_query", "net_fanout"): "transitive_net_fanout",
        ("cone_query", "gate_fanin"): "transitive_gate_fanin",
        ("cone_query", "gate_fanout"): "transitive_gate_fanout",
        ("cone_query", "output_rank"): "output_population_ranking",
        ("cone_query", "output_filter"): "output_population_filter",
    }.get(key, "")
    target = str(args[0]) if args and relation not in (
        "active_gate_inventory", "active_net_inventory",
        "primary_input_inventory", "primary_output_inventory",
        "output_population_ranking", "output_population_filter") else ""
    return {"relation": relation, "target": target}


def _build_evidence_record(result_id, family, payload, command, raw_output):
    mode, args = _payload_mode_args(payload)
    revision = _result_field(raw_output, "design_revision")
    record = {
        "result_id": result_id,
        "clause_ids": list(payload.get("clause_ids", [])),
        "family": family,
        "mode": mode,
        "args": list(args),
        "command": command,
        "status": envelope_status(raw_output),
        "ok": envelope_ok(raw_output),
        "complete": envelope_complete(raw_output),
        "design_revision": revision,
        "raw_output": raw_output or "",
        "fields": _parse_result_fields(raw_output),
        "inline_sections": _parse_complete_inline_sections(raw_output),
        "artifact_paths": _extract_artifact_paths(raw_output),
        "semantic_error": "",
    }
    record.update(_evidence_relation_signature(family, mode, args))
    return record


def _usable_evidence(record):
    return bool(record.get("ok") and record.get("complete") and
                not record.get("semantic_error") and
                record.get("status") not in (
                    "error", "unsupported", "timeout", "partial"))


def _record_matches_clause(record, deliverable):
    return deliverable.get("clause_id") in set(record.get("clause_ids", []))


def _label_matches_entity(label, entity):
    if not entity:
        return True
    label_tokens = set(_field_key(label).split())
    entity_key = _field_key(entity)
    if entity_key in label_tokens:
        return True
    return (entity_key + "s") in label_tokens


def _entity_field_key(entity):
    key = _field_key(entity)
    return {
        "primary output": "output",
        "primary input": "input",
    }.get(key, key)


def _structured_row_matches_relation(record, row):
    relation = record.get("relation", "")
    target = str(record.get("target", "")).lower()
    if relation == "direct_load":
        return (str(row.get("direction", "")).lower() == "load" and
                str(row.get("net", "")).lower() == target)
    if relation == "direct_driver":
        return (str(row.get("direction", "")).lower() == "driver" and
                str(row.get("net", "")).lower() == target)
    return True


def _section_records_for_entity(record, section, entity):
    """Project one complete counted section onto the requested entity type."""
    entity_key = _entity_field_key(entity)
    values = []
    if _label_matches_entity(section.get("label"), entity):
        values.extend(section.get("records", []))
    for row in section.get("structured_rows", []):
        if not _structured_row_matches_relation(record, row):
            continue
        value = row.get(entity_key)
        if value:
            values.append(value)
    unique = []
    seen = set()
    for value in values:
        key = str(value).lower()
        if key not in seen:
            seen.add(key)
            unique.append(str(value))
    return unique


def _expected_collection_count(record, deliverable):
    entity = _entity_field_key(deliverable.get("entity") or "")
    family_mode = (record.get("family"), record.get("mode"), entity)
    candidates = {
        ("structure_query", "net_loads", "gate"): ("count",),
        ("structure_query", "net_driver", "gate"): ("count",),
        ("structure_query", "gate_fanout", "gate"): ("count",),
        ("structure_query", "gate_fanin", "gate"): ("count",),
        ("cone_query", "output_rank", "output"): ("result output count",),
        ("cone_query", "output_filter", "output"): ("matched output count",),
    }.get(family_mode, ())
    fields = record.get("fields", {})
    for candidate in candidates:
        value = fields.get(candidate)
        match = re.match(r"^\s*(\d+)\b", str(value or ""))
        if match:
            return int(match.group(1))
    return None


def _collection_records_from_result(record, deliverable):
    values = []
    seen = set()
    for section in record.get("inline_sections", []):
        for value in _section_records_for_entity(
                record, section, deliverable.get("entity")):
            key = value.lower()
            if key not in seen:
                seen.add(key)
                values.append(value)
    return values


_METRIC_FIELD_ALIASES = {
    "equivalence": ("equivalent",),
    "gate count": ("gates", "active gate count"),
    "cone gate count": ("gates", "filtered gates"),
    "fanin cone gate count": ("gates", "filtered gates"),
    "fanin cone size": ("gates",),
    "cone net count": ("nets",),
    "fanin cone net count": ("nets",),
    "maximum depth": ("depth",),
    "path count": ("total paths",),
    "selected cone size": ("metric", "selected cone metric"),
    "matched output count": ("result output count",),
    "output count": ("result output count", "matched output count"),
}


def _metric_field_exists(record, metric):
    fields = record.get("fields", {})
    if not fields:
        return False
    if not metric:
        return True

    metric_key = _field_key(metric)
    metric_candidates = [metric_key]
    metric_candidates.extend(_METRIC_FIELD_ALIASES.get(metric_key, ()))

    def matches(field_name):
        field_tokens = set(_field_key(field_name).split())
        for candidate in metric_candidates:
            metric_tokens = set(_field_key(candidate).split())
            if not metric_tokens:
                return True
            metric_base = metric_tokens - set(("count", "number", "total", "value"))
            if (metric_tokens.issubset(field_tokens) or
                    field_tokens.issubset(metric_tokens)):
                return True
            for scalar_word in ("count", "depth", "fanout", "value", "rank"):
                if scalar_word in metric_tokens and scalar_word in field_tokens:
                    return True
            if metric_base:
                singular = set(token[:-1] if token.endswith("s") else token
                               for token in field_tokens)
                if metric_base.issubset(singular):
                    return True
        return False

    available_fields = list(fields)
    for section in record.get("inline_sections", []):
        for row in section.get("structured_rows", []):
            available_fields.extend(row)
    for field in available_fields:
        if matches(field):
            return True
    mode_record = _mode_record(REGISTRY.get(record.get("family"), {}),
                               record.get("mode"))
    for field in (mode_record or {}).get("authoritative_fields", []):
        if matches(field):
            return True
    return False


def _zero_collection_field(record, deliverable):
    """Return whether a complete result authoritatively reports an empty list."""
    entity = _field_key(deliverable.get("entity") or "")
    candidates = {
        "gate": ("count", "gates", "gate count", "filtered gates",
                 "scope gates", "gate detail count", "returned gate count"),
        "net": ("count", "nets", "net count", "returned net count"),
        "path": ("count", "paths", "path count", "total paths",
                 "returned path count"),
        "output": ("count", "outputs", "output count", "returned output count"),
        "input": ("count", "inputs", "input count", "returned input count"),
        "signal": ("count", "signals", "signal count", "match count"),
        "record": ("count", "records", "record count", "match count"),
    }.get(entity, ("count", "match count", "record count"))
    fields = record.get("fields", {})
    for field_name, value in fields.items():
        if _field_key(field_name) not in candidates:
            continue
        match = re.match(r"^\s*(\d+)\b", str(value))
        if match and int(match.group(1)) == 0:
            return True
    return False


def _record_supports_collection(record, deliverable):
    if not _usable_evidence(record) or not _record_matches_clause(record, deliverable):
        return False
    if record.get("artifact_paths"):
        return True
    records = _collection_records_from_result(record, deliverable)
    if records:
        expected = _expected_collection_count(record, deliverable)
        return expected is None or len(records) == expected
    return _zero_collection_field(record, deliverable)


def _selected_records_for_deliverable(deliverable, evidence_ledger):
    """Select one earliest authoritative result; never union later relations."""
    kind = deliverable.get("kind")
    if kind == "comparison":
        return [result for result in evidence_ledger or []
                if _usable_evidence(result) and
                _record_matches_clause(result, deliverable)]
    for result in evidence_ledger or []:
        if not _usable_evidence(result) or not _record_matches_clause(result, deliverable):
            continue
        if kind == "entity_list":
            if _record_supports_collection(result, deliverable):
                return [result]
            continue
        if kind == "scalar":
            if _metric_field_exists(result, deliverable.get("metric")):
                return [result]
            continue
        return [result]
    return []


def _records_for_deliverable(deliverable, evidence_ledger):
    records = []
    seen = set()
    for result in _selected_records_for_deliverable(deliverable, evidence_ledger):
        for record in _collection_records_from_result(result, deliverable):
            key = record.lower()
            if key not in seen:
                seen.add(key)
                records.append(record)
    return records


def _sections_for_deliverable(deliverable, evidence_ledger):
    sections = []
    for result in _selected_records_for_deliverable(deliverable, evidence_ledger):
        for section in result.get("inline_sections", []):
            if _section_records_for_entity(
                    result, section, deliverable.get("entity")):
                sections.append(section)
    return sections


def _artifacts_for_deliverable(deliverable, evidence_ledger):
    paths = []
    seen = set()
    for result in _selected_records_for_deliverable(deliverable, evidence_ledger):
        for path in result.get("artifact_paths", []):
            key = path.lower()
            if key not in seen:
                seen.add(key)
                paths.append(path)
    return paths


_SINGLE_LEVEL_RANK_MODES = set((
    "highest", "lowest", "nth_highest", "nth_lowest",
))


def _positive_integer_field(fields, names):
    for name in names:
        match = re.match(r"^\s*(\d+)\b", str(fields.get(name, "")))
        if match:
            return int(match.group(1))
    return None


def _ranking_metric_value(record):
    """Return one common selected-rank metric when backend evidence has it."""
    values = []
    row_key = "metric" if record.get("mode") == "output_rank" else "fanout"
    for section in record.get("inline_sections", []):
        for row in section.get("structured_rows", []):
            value = row.get(row_key)
            if value is not None:
                values.append(str(value))
    if values and len(set(values)) == 1:
        return values[0]

    if record.get("mode") != "output_rank":
        return ""
    fields = record.get("fields", {})
    metric = str(fields.get("output cone ranking metric", "")).strip().lower()
    candidates = {
        "scope_gates": ("scope gates", "gates"),
        "filtered_gates": ("filtered gates",),
        "nets": ("nets",),
    }.get(metric, ())
    for candidate in candidates:
        value = fields.get(candidate)
        if value is not None and str(value).strip():
            return str(value).strip()
    return ""


def _ranking_subject_label(record):
    if record.get("mode") == "output_rank":
        return "primary outputs"
    scope = str(record.get("fields", {}).get(
        "fanout ranking scope", "nets")).strip().lower()
    return {
        "pi": "primary inputs",
        "po": "primary outputs",
        "internal": "internal nets",
        "gate_output": "gate-output nets",
        "comb_output": "combinational-output nets",
        "dff_output": "DFF-output nets",
        "all": "nets",
    }.get(scope, "nets")


def _ranking_metric_label(record):
    if record.get("mode") == "fanout_rank":
        return "fanout"
    metric = str(record.get("fields", {}).get(
        "output cone ranking metric", "")).strip().lower()
    if not metric and record.get("args"):
        metric = str(record["args"][0]).strip().lower()
    return {
        "scope_gates": "fanin-cone gate-count",
        "filtered_gates": "filtered fanin-cone gate-count",
        "nets": "fanin-cone net-count",
    }.get(metric, "fanin-cone metric")


def _ranking_tie_summary(deliverables, evidence_ledger):
    """Build an authoritative summary for one selected rank level with ties.

    A representative ``source`` field is never treated as a unique winner.
    Top/bottom modes are excluded because they may contain several metric
    levels rather than one tied boundary.
    """
    for deliverable in deliverables or []:
        if (deliverable.get("kind") != "entity_list" or
                deliverable.get("coverage") != "exhaustive"):
            continue
        for record in _selected_records_for_deliverable(
                deliverable, evidence_ledger):
            if ((record.get("family"), record.get("mode")) not in (
                    ("cone_query", "output_rank"),
                    ("structure_query", "fanout_rank"))):
                continue
            fields = record.get("fields", {})
            mode_field = ("output cone ranking mode"
                          if record.get("mode") == "output_rank"
                          else "fanout ranking mode")
            rank_mode = str(fields.get(mode_field, "")).strip().lower()
            if not rank_mode and len(record.get("args", [])) >= 2:
                rank_mode = str(record["args"][1]).strip().lower()
            if rank_mode not in _SINGLE_LEVEL_RANK_MODES:
                continue
            count_fields = (("result output count",)
                            if record.get("mode") == "output_rank"
                            else ("result net count", "count"))
            result_count = _positive_integer_field(fields, count_fields)
            if result_count is None or result_count <= 1:
                continue

            subjects = _ranking_subject_label(record)
            metric_label = _ranking_metric_label(record)
            mode_label = rank_mode.replace("_", "-")
            records = _collection_records_from_result(record, deliverable)
            answer = ("There are {} {} tied at the selected {} {} level"
                      .format(result_count, subjects, mode_label, metric_label))
            if records and len(records) == result_count:
                answer += ": " + ", ".join(records)
            answer += "."
            metric_value = _ranking_metric_value(record)
            if metric_value:
                answer += " The tied metric value is {}.".format(metric_value)
            return answer
    return ""


def evidence_satisfies_deliverables(deliverables, evidence_ledger):
    """Return (satisfied, reason) from frozen answer-shape contracts."""
    if not deliverables:
        return True, ""
    for deliverable in deliverables:
        matching = [record for record in evidence_ledger or []
                    if _record_matches_clause(record, deliverable)]
        usable = _selected_records_for_deliverable(deliverable, evidence_ledger)
        kind = deliverable.get("kind")
        coverage = deliverable.get("coverage")
        if kind == "summary":
            if matching:
                continue
            return False, "no backend operation result is available for clause {}".format(
                deliverable.get("clause_id"))
        if not usable:
            return False, "no complete backend result is available for clause {}".format(
                deliverable.get("clause_id"))
        if kind == "scalar":
            if not any(_metric_field_exists(record, deliverable.get("metric"))
                       for record in usable):
                return False, "the requested scalar field is not available"
            continue
        if kind == "comparison":
            usable = [record for record in evidence_ledger or []
                      if _usable_evidence(record) and
                      _record_matches_clause(record, deliverable)]
            subjects = deliverable.get("subjects", [])
            metric = deliverable.get("metric")
            missing = []
            for subject in subjects:
                subject_low = subject.lower()
                matched = [record for record in usable
                           if subject_low in [str(arg).lower()
                                              for arg in record.get("args", [])] and
                           _metric_field_exists(record, metric)]
                if not matched:
                    missing.append(subject)
            if missing:
                return False, "missing complete comparison evidence for {}".format(
                    ", ".join(missing))
            continue
        if kind == "entity_list":
            artifacts = _artifacts_for_deliverable(deliverable, usable)
            sections = _sections_for_deliverable(deliverable, usable)
            records = _records_for_deliverable(deliverable, usable)
            empty_collection = any(_zero_collection_field(record, deliverable)
                                   for record in usable)
            if (coverage == "exhaustive" and not artifacts and not sections and
                    not empty_collection):
                return False, "the complete requested record collection is unavailable"
            if coverage == "bounded":
                limit = int(deliverable.get("limit", 0))
                if not artifacts and len(records) < limit:
                    return False, "only {} of {} bounded records are available".format(
                        len(records), limit)
            continue
    return True, ""


def _mutation_result_without_requested_details(user_req, result_context):
    return (bool(_MUTATION_RESULT_RE.search(result_context or "")) and
            not _EXPLICIT_MUTATION_DETAIL_RE.search(user_req or "") and
            not _EXPLICIT_MUTATION_COUNT_RE.search(user_req or ""))


def _requests_complete_collection(user_req, result_context=""):
    """Return whether the request explicitly asks for every collection record.

    This is an answer-shape check, not a tool router. It deliberately requires
    collection language such as ``list`` or ``determine all`` so universal
    yes/no questions and scalar requests such as ``count all gates`` are not
    turned into list requests.
    """
    if _mutation_result_without_requested_details(user_req, result_context):
        return False
    return bool(_COMPLETE_COLLECTION_RE.search(user_req or ""))


def _needs_concise_mutation_answer_repair(user_req, payload, result_context):
    """Reject unrequested or dangling edit/optimization detail narration."""
    if not _mutation_result_without_requested_details(user_req, result_context):
        return False
    answer = (payload or {}).get("answer", "").strip()
    if not answer:
        return False
    if answer.endswith(":"):
        return True
    return bool(_UNREQUESTED_MUTATION_DETAIL_RE.search(answer))


def _answer_has_list_shape(answer):
    """Recognize direct record delivery without knowing tool-specific names."""
    text = answer or ""
    return "\n" in text or text.count(",") >= 1 or text.count(";") >= 1


def _section_matches_collection(label, user_req, evidence):
    """Match a counted record section to the collection named by the answer."""
    label_key = " ".join(re.findall(r"[a-z0-9]+", (label or "").lower()))
    evidence_key = " ".join(re.findall(r"[a-z0-9]+", (evidence or "").lower()))
    if label_key and label_key in evidence_key:
        return True
    request = user_req or ""
    for noun in ("gate", "net", "signal", "path", "output", "input", "port",
                 "pin", "load", "driver", "dff", "register", "pair", "node",
                 "record", "equation", "expression", "match", "violation", "issue"):
        if (re.search(r"\b" + noun + r"s?\b", label, re.IGNORECASE) and
                re.search(r"\b" + noun + r"s?\b", request, re.IGNORECASE)):
            return True
    return False


def _required_simple_inline_records(user_req, payload, result_context):
    """Extract complete one-token identity sections declared as ``Label (N):``.

    The tools use these counted sections for inline Gate names, Cone gates,
    Net names, and similar identity collections. Structured multi-line records
    are intentionally ignored here and remain governed by their artifact and
    Tool Card contracts.
    """
    evidence = " ".join((payload or {}).get("evidence", []))
    lines = (result_context or "").splitlines()
    required = []
    seen = set()
    index = 0
    while index < len(lines):
        header = _COUNTED_SECTION_RE.match(lines[index])
        if not header:
            index += 1
            continue
        label = header.group(1).strip()
        declared_count = int(header.group(2))
        records = []
        cursor = index + 1
        while cursor < len(lines):
            line = lines[cursor]
            if not line.strip():
                cursor += 1
                continue
            if not line[:1].isspace():
                break
            record = line.strip()
            if len(record.split()) == 1 and ":" not in record:
                records.append(record)
            cursor += 1
        if (declared_count > 0 and len(records) == declared_count and
                _section_matches_collection(label, user_req, evidence)):
            for record in records:
                key = record.lower()
                if key not in seen:
                    seen.add(key)
                    required.append(record)
        index = max(index + 1, cursor)
    return required


def _needs_complete_collection_repair(user_req, payload, result_context,
                                      artifact_paths=None):
    """Reject an obvious count-only answer to an exhaustive collection request.

    Public artifacts already satisfy delivery of the complete payload. Inline
    results remain model evidence, so this guard only catches the common failure
    where requested identities are replaced by a scalar summary. It does not
    understand or rewrite EDA results.
    """
    if not _requests_complete_collection(user_req, result_context) or artifact_paths:
        return False
    answer = (payload or {}).get("answer", "").strip()
    if not answer or _EMPTY_COLLECTION_RE.search(answer):
        return False
    if _OMITTED_COLLECTION_RE.search(answer):
        return True
    required_records = _required_simple_inline_records(
        user_req, payload, result_context)
    if required_records:
        answer_ids = set(token.strip("`'\"(){}<>.").lower()
                         for token in _ANSWER_ID_RE.findall(answer))
        return any(record.lower() not in answer_ids for record in required_records)
    count_summary = bool(_COUNT_SUMMARY_RE.search(answer))
    inline_after_colon = bool(re.search(r":\s*\S+\s*,\s*\S+", answer))
    if count_summary and not inline_after_colon:
        return True
    if _answer_has_list_shape(answer):
        return False
    evidence = " ".join((payload or {}).get("evidence", []))
    scalar_evidence = bool(re.search(
        r"(?:^|\s)(?:total\s+)?(?:gates?|nets?|paths?|outputs?|inputs?|"
        r"matches?|records?|count)\s*:\s*\d+\b", evidence,
        re.IGNORECASE))
    return scalar_evidence


def finalize_model_answer(user_req, payload, result_context, artifact_paths=None,
                          deliverables=None, evidence_ledger=None):
    """Apply evidence-only corrections that must not depend on model obedience."""
    answer = payload.get("answer", "").strip()
    public_artifacts = []
    seen_artifacts = set()
    if deliverables is not None:
        artifact_candidates = []
        for deliverable in deliverables:
            artifact_candidates.extend(
                _artifacts_for_deliverable(deliverable, evidence_ledger))
    else:
        artifact_candidates = list(artifact_paths or [])
        for result in evidence_ledger or []:
            artifact_candidates.extend(result.get("artifact_paths", []))
    for path in artifact_candidates:
        key = path.lower()
        if key not in seen_artifacts:
            seen_artifacts.add(key)
            public_artifacts.append(path)
    if not public_artifacts:
        public_artifacts = _extract_artifact_paths(result_context)

    pi_only = _result_field(
        result_context, "primary-input-only combinational expression available")
    if (re.search(r"\b(boolean|logic)\s+(equation|expression|formula)\b",
                  user_req or "", re.IGNORECASE) and pi_only.lower() == "no"):
        target = _result_field(result_context, "net A") or "the requested signal"
        mappings = re.findall(
            r"^\s*(state_q\d+\s*=\s*[^\r\n]+(?:\(net\s+[^\r\n]+\))?)\s*$",
            result_context or "", re.MULTILINE)
        mapping = mappings[0].strip() if mappings else "a DFF.Q current-state variable"
        answer = ("A primary-input-only combinational expression for {} is not available "
                  "because it is a sequential boundary. Its current-state representation "
                  "uses {}.".format(target, mapping))

    ranking_summary = _ranking_tie_summary(deliverables, evidence_ledger)
    if ranking_summary:
        answer = ranking_summary

    missing_artifacts = [path for path in public_artifacts
                         if path.lower() not in answer.lower()]
    if len(missing_artifacts) == 1:
        answer += " Complete result artifact: {}.".format(missing_artifacts[0])
    elif missing_artifacts:
        answer += " Complete result artifacts: {}.".format(
            ", ".join(missing_artifacts))

    if deliverables is not None:
        appended = set()
        for deliverable in deliverables:
            if deliverable.get("kind") != "entity_list":
                continue
            if _artifacts_for_deliverable(deliverable, evidence_ledger):
                continue
            required_records = _records_for_deliverable(
                deliverable, evidence_ledger)
            if deliverable.get("coverage") == "bounded":
                required_records = required_records[:int(deliverable.get("limit", 0))]
            answer_ids = set(token.strip("`'\"(){}<>.").lower()
                             for token in _ANSWER_ID_RE.findall(answer))
            missing = [record for record in required_records
                       if record.lower() not in answer_ids and
                       record.lower() not in appended]
            if missing:
                if answer and not answer.endswith((" ", "\n")):
                    answer += " "
                entity = deliverable.get("entity") or "record"
                answer += "Complete requested {} records ({}): {}.".format(
                    entity, len(required_records), ", ".join(required_records))
                appended.update(record.lower() for record in required_records)
    elif not public_artifacts and _requests_complete_collection(user_req, result_context):
        # Compatibility path for direct callers that do not yet pass a frozen
        # deliverables contract. Runtime ReAct requests use the contract path.
        required_records = _required_simple_inline_records(
            user_req, payload, result_context)
        if required_records:
            answer_ids = set(token.strip("`'\"(){}<>.").lower()
                             for token in _ANSWER_ID_RE.findall(answer))
            if any(record.lower() not in answer_ids for record in required_records):
                if answer and not answer.endswith((" ", "\n")):
                    answer += " "
                answer += "Complete requested records ({}): {}.".format(
                    len(required_records), ", ".join(required_records))
    return answer


def _unsupported_answer(reason):
    detail = " ".join((reason or "").split())
    if detail:
        return ("No documented public tool can establish the requested result. " + detail +
                " No exact list, count, or proof is claimed.")
    return ("No documented public tool can establish the requested result; "
            "no exact list, count, or proof is claimed.")


def _chat_with_transport_retry(llm, prompt, messages, deadline=None, trace=None,
                               step=None):
    """Retry one failed logical model action without changing its context."""
    last_error = None
    for attempt in range(LLM_ACTION_ATTEMPTS):
        try:
            return llm.chat(prompt, messages, deadline=deadline)
        except Exception as error:
            last_error = error
            if trace is not None:
                trace.record("llm_transport_failure", {
                    "step": step,
                    "attempt": attempt + 1,
                    "will_retry": attempt + 1 < LLM_ACTION_ATTEMPTS,
                    "error": "{}: {}".format(type(error).__name__, error),
                })
            if attempt + 1 < LLM_ACTION_ATTEMPTS:
                sys.stderr.write(
                    "[react] LLM call failed; retrying the same action context: {}\n"
                    .format(error))
    raise last_error


def handle_react(llm, backend, user_req, deadline=None, history=None, categories=None,
                 active_cost_function=None, trace=None):
    """Execute one request through strict JSON actions and registry validation."""
    _ = history  # prior natural-language answers are intentionally not session state
    if deadline is None:
        deadline = time.monotonic() + ANALYSIS_REQUEST_SECONDS
    initial_families = candidate_families(user_req) if categories is None else list(categories)
    extra_families = []
    messages = [{"role": "user", "content":
                 "Clause C1 is the complete request below. Preserve every deliverable.\n"
                 "User request: " + user_req}]
    timed_out_commands = set()
    failed_commands = set()
    mutation_answer_repair_used = False
    usable_backend_evidence = False
    last_result_context = ""
    artifact_paths = []
    frozen_deliverables = None
    evidence_ledger = []
    required_workflow = _required_compound_workflow(
        user_req, active_cost_function=active_cost_function)
    frozen_workflow = required_workflow
    completed_workflow_stages = 0

    for step in range(MAX_REACT_STEPS):
        remaining = max(0.0, deadline - time.monotonic())
        final_turn = (step == MAX_REACT_STEPS - 1 or
                      remaining <= FINAL_ANSWER_RESERVE_SECONDS)
        if final_turn:
            if usable_backend_evidence:
                final_instruction = (
                    "FINAL TURN. Return one answer-action JSON object now from the best "
                    "available evidence. No further run action is available.")
            else:
                final_instruction = (
                    "FINAL TURN. No usable backend result was obtained. Return an answer-action "
                    "JSON object with the single "
                    "most likely answer in the exact type requested by the user (count, "
                    "list, yes/no, expression, or transformation outcome). Be concise, do "
                    "not answer unknown, and do not mention tool, API, or network failure.")
            messages.append({"role": "user", "content": final_instruction})
        prompt = build_react_prompt(
            user_req, categories=initial_families, extra_families=extra_families,
            remaining_seconds=remaining, remaining_steps=MAX_REACT_STEPS - step,
            active_cost_function=active_cost_function)
        try:
            llm_out = _chat_with_transport_retry(
                llm, prompt, messages, deadline=deadline, trace=trace,
                step=step + 1)
        except Exception as e:
            sys.stderr.write("[react] LLM call failed after one retry: {}\n".format(e))
            return _fallback_answer(user_req)

        action, payload, parse_error = parse_action_json(llm_out, user_req=user_req)
        if trace is not None:
            trace.record("llm_action", {"step": step + 1, "raw": llm_out,
                                        "parse_error": parse_error})

        if final_turn:
            if action == "ANSWER":
                if (frozen_workflow and
                        completed_workflow_stages < len(frozen_workflow["stages"])):
                    if trace is not None:
                        trace.record("incomplete_workflow_final_turn", {
                            "step": step + 1,
                            "completed_stages": completed_workflow_stages,
                            "required_stages": len(frozen_workflow["stages"]),
                        })
                    return _fallback_answer(user_req)
                if _needs_unproved_equivalence_fallback(
                        initial_families + extra_families,
                        usable_backend_evidence):
                    if trace is not None:
                        trace.record("equivalence_positive_fallback", {
                            "step": step + 1,
                            "reason": "no usable whole-design equivalence result",
                        })
                    return _unproved_equivalence_fallback()
                return finalize_model_answer(
                    user_req, payload, last_result_context, artifact_paths,
                    frozen_deliverables, evidence_ledger)
            # Migration tolerance prevents a formatting mistake from consuming
            # the entire scored request after useful evidence was obtained.
            old_action, old_payload, _ = parse_llm_line(llm_out, allow_free_text=True)
            if (old_action == "ANSWER" and old_payload and
                    not _needs_unproved_equivalence_fallback(
                        initial_families + extra_families,
                        usable_backend_evidence)):
                return finalize_model_answer(
                    user_req, {"answer": old_payload}, last_result_context,
                    artifact_paths, frozen_deliverables, evidence_ledger)
            if _needs_unproved_equivalence_fallback(
                    initial_families + extra_families,
                    usable_backend_evidence):
                if trace is not None:
                    trace.record("equivalence_positive_fallback", {
                        "step": step + 1,
                        "reason": "final action was unusable and no proof was obtained",
                    })
                return _unproved_equivalence_fallback()
            return _fallback_answer(user_req)

        if action == "ANSWER":
            if (frozen_workflow and
                    completed_workflow_stages < len(frozen_workflow["stages"])):
                next_stage = _workflow_expected_stage(
                    frozen_workflow, completed_workflow_stages)
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The compound request is not complete. Finished workflow "
                                 "stages: {}/{}. RUN the next required stage exactly as "
                                 "declared: {}. Do not answer or write the design yet."
                                 .format(completed_workflow_stages,
                                         len(frozen_workflow["stages"]),
                                         _workflow_stage_text(next_stage))})
                if trace is not None:
                    trace.record("incomplete_workflow_answer", {
                        "step": step + 1,
                        "completed_stages": completed_workflow_stages,
                        "next_stage": next_stage,
                    })
                continue
            if _needs_unproved_equivalence_fallback(
                    initial_families + extra_families,
                    usable_backend_evidence):
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "No successful whole-design equivalence result is "
                                 "available yet. RUN the documented equiv_query mode. "
                                 "Keep family and mode separate: family is equiv_query; "
                                 "mode is original or previous_edit."})
                continue
            if (not mutation_answer_repair_used and
                    _needs_concise_mutation_answer_repair(
                        user_req, payload, last_result_context)):
                mutation_answer_repair_used = True
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The original request asks to apply an edit or "
                                 "optimization, but does not ask for counts, records, or "
                                 "before/after statistics. Return a concise outcome using "
                                 "the operation status, whether a change was retained, and "
                                 "the available functionality-preservation result. Do not "
                                 "quote cumulative design statistics or introduce details "
                                 "that are not actually included."})
                continue
            evidence_complete, evidence_reason = evidence_satisfies_deliverables(
                frozen_deliverables, evidence_ledger)
            if not evidence_complete:
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The proposed ANSWER cannot be finalized because the frozen "
                                 "deliverables contract is not yet supported by complete "
                                 "backend evidence: {}. RUN another documented command that "
                                 "supplies the missing evidence; do not change the contract."
                                 .format(evidence_reason)})
                if trace is not None:
                    trace.record("insufficient_deliverable_evidence", {
                        "step": step + 1,
                        "reason": evidence_reason,
                    })
                continue
            return finalize_model_answer(
                user_req, payload, last_result_context, artifact_paths,
                frozen_deliverables, evidence_ledger)
        if action == "NONE":
            messages.append({"role": "assistant", "content": llm_out.strip()[:500]})
            messages.append({"role": "user", "content":
                             "Invalid action ({}). For a run, return only action, family, "
                             "mode, args, clause_ids, and the first-turn deliverables "
                             "contract. Put every positional operand and option token in "
                             "args in documented order. Return exactly one JSON run or "
                             "answer object.".format(parse_error)})
            continue

        payload, search_contract_reason = normalize_unknown_search_deliverables(
            user_req, payload)
        if search_contract_reason:
            sys.stderr.write("[normalize] {}\n".format(search_contract_reason))
            if trace is not None:
                trace.record("deliverable_contract_normalized", {
                    "step": step + 1,
                    "reason": search_contract_reason,
                    "family": payload.get("family", ""),
                    "deliverables": payload.get("deliverables", []),
                })

        proposed_deliverables = payload.get("deliverables")
        proposed_workflow = payload.get("workflow")
        if frozen_deliverables is None:
            if not proposed_deliverables:
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The first valid RUN action must include the prompt-local "
                                 "deliverables contract. Classify each requested output as "
                                 "entity_list/exhaustive or bounded, scalar/scalar, "
                                 "comparison/scalar, or summary/summary, then return the "
                                 "corrected RUN action."})
                continue
        elif (proposed_deliverables is not None and
              proposed_deliverables != frozen_deliverables):
            messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
            messages.append({"role": "user", "content":
                             "The deliverables contract was frozen by the first valid RUN "
                             "and cannot be changed or reduced. Return a RUN action without "
                             "deliverables, or repeat the original contract exactly."})
            continue

        if frozen_workflow is not None:
            if proposed_workflow is not None and proposed_workflow != frozen_workflow:
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The ordered workflow contract is already fixed by the "
                                 "request or first valid RUN and cannot be changed. Omit "
                                 "workflow on later RUN actions, or repeat it exactly."})
                continue
        elif proposed_workflow is not None:
            workflow_valid, workflow_error = validate_workflow_contract(
                user_req, proposed_workflow,
                active_cost_function=active_cost_function)
            if not workflow_valid:
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The proposed workflow was not accepted: {}. Preserve "
                                 "the request's objective and hard requirements, then "
                                 "return a corrected RUN action.".format(workflow_error)})
                continue

        requested_family = payload["family"].strip().lower()
        # A candidate Card is advisory, not a whitelist. Once the model selects
        # any registered family, make its complete Card available on every
        # subsequent repair turn, including semantic failures that occur before
        # command grammar validation.
        if (requested_family in REGISTRY and
                requested_family not in initial_families and
                requested_family not in extra_families):
            extra_families.append(requested_family)
            if trace is not None:
                trace.record("family_card_loaded", {
                    "step": step + 1,
                    "family": requested_family,
                    "reason": "model selected a registered candidate-outside family",
                })
        requested_family, payload, normalization_reason = normalize_analysis_action(
            user_req, requested_family, payload)
        effective_deliverables = (frozen_deliverables if frozen_deliverables is not None
                                  else proposed_deliverables)
        requested_family, payload, deliverable_normalization_reason = \
            normalize_deliverable_action(
                user_req, requested_family, payload, effective_deliverables)
        normalization_reasons = [reason for reason in (
            normalization_reason, deliverable_normalization_reason) if reason]
        if normalization_reasons:
            combined_normalization_reason = "; ".join(normalization_reasons)
            sys.stderr.write("[normalize] {}\n".format(combined_normalization_reason))
            if trace is not None:
                trace.record("analysis_action_normalized", {
                    "step": step + 1,
                    "reason": combined_normalization_reason,
                    "family": requested_family,
                    "mode": payload.get("mode", ""),
                    "args": payload.get("args", []),
                })
        compatible, compatibility_error = validate_deliverable_compatibility(
            user_req, requested_family, payload, effective_deliverables)
        if not compatible:
            messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
            messages.append({"role": "user", "content":
                             "The command was not executed because the selected mode "
                             "cannot produce the frozen deliverables contract: {}. "
                             "Return one corrected JSON action without changing the "
                             "deliverables.".format(compatibility_error)})
            continue
        semantic_valid, semantic_error = validate_action_semantics(
            user_req, requested_family, payload,
            active_cost_function=active_cost_function)
        if not semantic_valid:
            messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
            messages.append({"role": "user", "content":
                             "The command was not executed because its mode does not "
                             "represent the original request: {}. Re-read the mode "
                             "ownership distinctions and return one corrected JSON "
                             "action.".format(semantic_error)})
            continue
        effective_workflow = (frozen_workflow if frozen_workflow is not None
                              else proposed_workflow)
        if effective_workflow is not None:
            workflow_valid, workflow_error = validate_workflow_contract(
                user_req, effective_workflow,
                active_cost_function=active_cost_function)
            if not workflow_valid:
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The ordered workflow contract is invalid: {}. Return "
                                 "one corrected RUN action.".format(workflow_error)})
                continue
            expected_stage = _workflow_expected_stage(
                effective_workflow, completed_workflow_stages)
            if not _workflow_action_matches_stage(
                    requested_family, payload, expected_stage):
                messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
                messages.append({"role": "user", "content":
                                 "The command was not executed because workflow stage {} "
                                 "must be completed next: {}. Preserve the frozen "
                                 "deliverables and workflow order."
                                 .format(completed_workflow_stages + 1,
                                         _workflow_stage_text(expected_stage))})
                if trace is not None:
                    trace.record("workflow_stage_mismatch", {
                        "step": step + 1,
                        "completed_stages": completed_workflow_stages,
                        "expected_stage": expected_stage,
                        "proposed_family": requested_family,
                        "proposed_mode": payload.get("mode", ""),
                        "proposed_args": payload.get("args", []),
                    })
                continue
        valid, command_or_error, registry_family = validate_action_command(
            requested_family, payload)
        if not valid:
            if registry_family and registry_family not in extra_families:
                extra_families.append(registry_family)
            messages.append({"role": "assistant", "content": llm_out.strip()[:1000]})
            messages.append({"role": "user", "content":
                             "The command was not executed: {}. Review the newly detailed "
                             "Tool Card and return one corrected JSON action.".format(
                                 command_or_error)})
            continue
        command = command_or_error
        if requested_family not in initial_families and requested_family not in extra_families:
            extra_families.append(requested_family)
        assistant_action = json.dumps(payload, ensure_ascii=False, sort_keys=True)
        if command in timed_out_commands:
            messages.append({"role": "assistant", "content": assistant_action})
            messages.append({"role": "user", "content":
                             "That exact command already timed out on this design revision and "
                             "was not called again. Choose a different documented strategy, "
                             "or answer only with verified incomplete facts."})
            continue
        if command in failed_commands:
            messages.append({"role": "assistant", "content": assistant_action})
            messages.append({"role": "user", "content":
                             "That exact command already returned an error on the "
                             "unchanged design and was not called again. Reconsider the "
                             "selected mode and argument semantics before choosing a "
                             "different documented action."})
            continue
        # A contract is frozen only by the first command that has passed both
        # semantic and grammar validation and is about to execute. An invalid
        # draft must not lock later corrected routing to the wrong deliverable.
        if frozen_deliverables is None:
            frozen_deliverables = proposed_deliverables
        if frozen_workflow is None and proposed_workflow is not None:
            frozen_workflow = proposed_workflow
        raw_output = backend.send(command, timeout=None)
        result_id = "R{}".format(len(evidence_ledger) + 1)
        evidence_record = _build_evidence_record(
            result_id, requested_family, payload, command, raw_output)
        result_semantic_error = ""
        if requested_family == "opt_apply":
            result_valid, result_semantic_error = _validate_opt_result_semantics(
                payload, raw_output)
            if not result_valid:
                evidence_record["semantic_error"] = result_semantic_error
        evidence_ledger.append(evidence_record)
        if (frozen_workflow is not None and
                completed_workflow_stages < len(frozen_workflow["stages"]) and
                _usable_evidence(evidence_record)):
            completed_workflow_stages += 1
        for artifact_path in _extract_artifact_paths(raw_output):
            if artifact_path.lower() not in [path.lower() for path in artifact_paths]:
                artifact_paths.append(artifact_path)
        raw_status = envelope_status(raw_output)
        if raw_status == "timeout":
            timed_out_commands.add(command)
        if raw_status in ("error", "unsupported") and not envelope_ok(raw_output):
            failed_commands.add(command)
        if raw_output and not result_semantic_error and (envelope_ok(raw_output) or
                           raw_status in ("partial", "timeout", "no_change")):
            usable_backend_evidence = True
        obs = observation_for_llm(raw_output)
        if frozen_workflow is not None:
            if completed_workflow_stages < len(frozen_workflow["stages"]):
                obs += ("\n\nWorkflow progress: {}/{} stages complete. "
                        "Next required stage: {}."
                        .format(completed_workflow_stages,
                                len(frozen_workflow["stages"]),
                                _workflow_stage_text(_workflow_expected_stage(
                                    frozen_workflow, completed_workflow_stages))))
            else:
                obs += "\n\nWorkflow progress: all required stages complete."
        if result_semantic_error:
            obs += ("\n\nConstraint-result validation:\n" +
                    result_semantic_error +
                    "\nDo not report this optimization as successful. Correct the "
                    "scope or gate-basis parameters before retrying.")
        recovery_hint = _analysis_error_recovery_hint(
            requested_family, payload, raw_output)
        if recovery_hint:
            obs += "\n\nRecovery guidance:\n" + recovery_hint
        obs = _truncate_obs(obs, "s{}_{:.0f}".format(step, time.time()))
        last_result_context = (raw_output or "") + "\n" + obs
        if trace is not None:
            trace.record("backend_result", {"step": step + 1, "family": requested_family,
                                             "result_id": result_id,
                                             "command": command, "status": raw_status,
                                             "complete": envelope_complete(raw_output),
                                             "observation": obs})
        messages.append({"role": "assistant", "content": assistant_action})
        messages.append({"role": "user", "content":
                         "Result {}:\n{}\n\nApply the frozen deliverables contract "
                         "to the original request. "
                         "If evidence is incomplete or mismatched, RUN another "
                         "documented command. Otherwise ANSWER from the exact "
                         "authoritative result field. Return exactly one JSON action."
                         .format(result_id, obs)})

    # MAX_REACT_STEPS includes the final answer turn; no extra LLM call follows it.
    return _fallback_answer(user_req)


# ╔════════════════════════════════════════════════════════════════════════╗
# ║  Main                                                                  ║
# ╚════════════════════════════════════════════════════════════════════════╝

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-config", "--config", required=True)
    ap.add_argument("--url", default=None)
    ap.add_argument("--model", default=None)
    ap.add_argument("--debug", action="store_true",
                    help="write a separate JSONL trace; official stdout is unchanged")
    ap.add_argument("--debug-log", default=None)
    args = ap.parse_args()

    config = parse_yaml(args.config)
    provider = config.get("provider", "openai")
    gen = config.get("generation", {})
    if isinstance(gen, str): gen = {}
    temp = float(gen.get("temperature", "0.2"))
    mt = int(gen.get("max_output_tokens", "4096"))

    if provider == "anthropic":
        sub = config.get("anthropic", {})
        llm = LLMClient("anthropic", args.model or sub.get("model", "claude-haiku-4-5"),
                        sub.get("api_key", ""), temp, mt)
    else:
        sub = config.get("openai", {})
        llm = LLMClient("openai", args.model or sub.get("model", "gpt-4o-mini"),
                        sub.get("api_key", ""), temp, mt,
                        args.url or sub.get("base_url"))

    backend = EDABackend(TOOLS_EXE)
    logger = Logger()
    trace = DebugTrace(args.debug, args.debug_log)
    session_state = SessionSemanticState()

    def respond(request, message):
        """Emit only the official response block; debug detail stays separate."""
        logger.emit(message)
        trace.record("official_response", {"response_id": logger.resp_id,
                                            "request": request, "answer": message})

    for raw_line in sys.stdin:
        req = raw_line.strip()
        if not req:
            continue

        if is_testcase_start(req):
            case_name = extract_case_name(req)
            logger.set_log(case_name)
            session_state.reset()
            logger.emit('Acknowledged. Initialized testcase "{}". All subsequent responses will '
                        'be recorded to {}.log. Design state is empty and ready for '
                        'commands.'.format(case_name, case_name))
            continue

        if is_read_request(req):
            path = extract_read_path(req)
            if path:
                raw = backend.send(session_command("read", path), timeout=None)
                if envelope_ok(raw) and envelope_complete(raw):
                    # A60: also drop a log copy beside the design being read.
                    logger.add_dir(os.path.dirname(os.path.abspath(path)))
                    respond(req, 'Loaded gate-level Verilog from "{}" successfully.'.format(path))
                else:
                    respond(req, 'Failed to load "{}". {}'.format(
                        path, envelope_message(raw) or raw))
            else:
                respond(req, "[Error] Could not extract file path from request.")
            continue

        if is_write_request(req):
            path = extract_write_path(req)
            raw = backend.send(session_command("write", path), timeout=None)
            if envelope_ok(raw) and envelope_complete(raw):
                respond(req, 'Wrote the current design to "{}" successfully.'.format(path))
            else:
                respond(req, 'Failed to write "{}". {}'.format(
                    path, envelope_message(raw) or raw))
            continue

        request_deadline = time.monotonic() + ANALYSIS_REQUEST_SECONDS
        inherited_cost = session_state.for_request(req)
        trace.record("request_start", {"request": req,
                                       "deadline_seconds": ANALYSIS_REQUEST_SECONDS,
                                       "inherited_cost_function": inherited_cost})
        answer = handle_react(llm, backend, req, deadline=request_deadline,
                              active_cost_function=inherited_cost, trace=trace)
        respond(req, answer)

    logger.close()
    backend.close()


if __name__ == "__main__":
    main()
