"""callfilter.py — the local skimmer's callsign filter, by time.

skimmer-headless drops every call its [filter] does not let through before
the call reaches its feed (take_text, right after the extractor). CW Skimmer
Server has no such filter, so without this W would be credited with the
stations L is not allowed to spot (the Russian/Belarusian default) and L
would count each of them as a miss. compare.ini [local filter] holds
"<UTC since> = regex" lines, each in force until the next; an empty regex is
no filter. Inside a filter's time a spot of either side whose call the
filter refuses is left out of the comparison — L's own spots there are
already filtered, so for L it changes nothing unless the filter changed.

The regex is matched as headless matches it: GRegex with G_REGEX_CASELESS,
g_regex_match — a search anywhere in the call, any case.
"""
import re
from bisect import bisect_right

from .windows import parse_since


def filter_plan(cp):
    """[(since epoch, compiled regex or None)] from compare.ini, sorted."""
    plan = []
    if cp.has_section("local filter"):
        for k, v in cp.items("local filter", raw=True):
            t = parse_since(k)
            if t is None:
                continue
            v = v.strip()
            try:
                plan.append((t, re.compile(v, re.I) if v else None))
            except re.error as e:
                raise SystemExit("compare.ini [local filter] %s: %s" % (k, e))
    return sorted(plan, key=lambda x: x[0])


class CallFilter:
    """passes(t, call): would the local skimmer's filter at t let call out?"""

    def __init__(self, plan):
        self.plan = plan or []
        self.since = [s for s, _ in self.plan]
        self.memo = [dict() for _ in self.plan]

    def __bool__(self):
        return any(r is not None for _, r in self.plan)

    def passes(self, t, call):
        i = bisect_right(self.since, t) - 1
        if i < 0 or self.plan[i][1] is None:
            return True
        m = self.memo[i]
        v = m.get(call)
        if v is None:
            v = m[call] = self.plan[i][1].search(call) is not None
        return v

    def describe(self):
        """[(since, pattern or "")] — for the page."""
        return [(s, r.pattern if r is not None else "") for s, r in self.plan]
