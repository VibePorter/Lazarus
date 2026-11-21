"""Greedy LZO1X compressor (M2/M3/M4 matches, literal runs, standard end marker). Output is decodable by any
LZO1X decompressor (verified against me2_package.lzo1x_decompress)."""

MAX_DIST = 0xBFFF


def _ext(out, n):
    while n > 255:
        out.append(0)
        n -= 255
    out.append(n)


def compress(src):
    src = bytes(src)
    n = len(src)
    out = bytearray()
    table = {}
    lit_start = 0
    i = 0
    last_state_pos = -1  # index in out of the byte holding the previous match's state bits
    first = True

    def flush_literals(end):
        nonlocal first, last_state_pos
        run = end - lit_start
        if run == 0:
            return
        if first and run <= 238:
            out.append(17 + run)
        elif run <= 3 and last_state_pos >= 0:
            out[last_state_pos] |= run
        elif run <= 18:
            out.append(run - 3)
        else:
            out.append(0)
            _ext(out, run - 18)
        out.extend(src[lit_start:end])
        first = False

    while i + 3 <= n:
        key = src[i:i + 3]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= MAX_DIST:
            dist = i - cand
            ln = 3
            while i + ln < n and src[cand + ln] == src[i + ln]:
                ln += 1
            run = i - lit_start
            # a 1..3 literal run can only ride on a previous match's state bits
            if 0 < run <= 3 and last_state_pos < 0 and not first:
                i += 1
                continue
            flush_literals(i)
            first = False
            if 3 <= ln <= 8 and dist <= 0x800:
                out.append(((ln - 1) << 5) | (((dist - 1) & 7) << 2))
                last_state_pos = len(out) - 1
                out.append((dist - 1) >> 3)
            elif dist <= 0x4000:
                if ln - 2 <= 31:
                    out.append(32 | (ln - 2))
                else:
                    out.append(32)
                    _ext(out, ln - 2 - 31)
                v = (dist - 1) << 2
                last_state_pos = len(out)
                out.append(v & 0xFF)
                out.append(v >> 8)
            else:
                d = dist - 0x4000
                hi = 8 if d >= 0x4000 else 0
                if ln - 2 <= 7:
                    out.append(16 | hi | (ln - 2))
                else:
                    out.append(16 | hi)
                    _ext(out, ln - 2 - 7)
                v = (d & 0x3FFF) << 2
                last_state_pos = len(out)
                out.append(v & 0xFF)
                out.append(v >> 8)
            for k in range(i + 1, min(i + ln, n - 2)):
                table[src[k:k + 3]] = k
            i += ln
            lit_start = i
        else:
            i += 1
    run = n - lit_start
    if 0 < run <= 3 and last_state_pos < 0 and not first:
        raise AssertionError('unreachable')
    flush_literals(n)
    out += b'\x11\x00\x00'
    return bytes(out)
