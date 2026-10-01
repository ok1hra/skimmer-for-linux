"""intervals.py — sorted, non-overlapping [(a, b)] time intervals."""
from bisect import bisect_right


def normalize(iv):
    out = []
    for a, b in sorted(x for x in iv if x[1] > x[0]):
        if out and a <= out[-1][1]:
            if b > out[-1][1]:
                out[-1] = (out[-1][0], b)
        else:
            out.append((a, b))
    return out


def intersect(x, y):
    x, y = normalize(x), normalize(y)
    out, i, j = [], 0, 0
    while i < len(x) and j < len(y):
        a, b = max(x[i][0], y[j][0]), min(x[i][1], y[j][1])
        if b > a:
            out.append((a, b))
        if x[i][1] < y[j][1]:
            i += 1
        else:
            j += 1
    return out


def subtract(x, y):
    out = []
    y = normalize(y)
    for a, b in normalize(x):
        cur = a
        for c, d in y:
            if d <= cur or c >= b:
                continue
            if c > cur:
                out.append((cur, c))
            cur = max(cur, d)
            if cur >= b:
                break
        if cur < b:
            out.append((cur, b))
    return out


def clip(x, a, b):
    return intersect(x, [(a, b)])


def total(x):
    return sum(b - a for a, b in x)


class Membership:
    """Fast 'is t inside' for a normalized interval list."""

    def __init__(self, iv):
        self.iv = normalize(iv)
        self.starts = [a for a, _ in self.iv]

    def __contains__(self, t):
        i = bisect_right(self.starts, t) - 1
        return i >= 0 and t <= self.iv[i][1]
