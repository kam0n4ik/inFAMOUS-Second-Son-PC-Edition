# SecondSonPC P4.14: what the persistent shader caches of several scenes say about predicting a scene's pipelines
# before its first visit (docs\OFFLINE_SHADER_PRECOMPILATION.md). Reads shaders.pack files (src\runtime\gpu\shader\
# shader_disk_cache.h) and, when given, the game's program index (game_programs.idx, --scan-game-shaders).
#   py -I -X utf8 tools\p414\pack_analyze.py --scene village=<pack> --scene beach=<pack> ... [--index <idx>] [--out x.md]
# For each scene X against the union of the others ("the rest" = what earlier visits elsewhere would have cached):
#   programs, modules (SPIR-V), pipelines of X already known from the rest (Exact for X);
#   new pipelines of X: both stages' SPIR-V known elsewhere and only the combination new (a known-module x
#   known-state prediction could hit) / the state and bindings seen elsewhere;
#   new modules of X: their runtime parameters seen elsewhere (exactly, or apart from RSRC1/RSRC2 which are per
#   program), their specialization key's words seen elsewhere (the descriptors' formats) - an upper bound of what a
#   template-based translation could reproduce exactly.
import collections
import struct
import sys

REC_MAGIC = 0x43525053


def records(path):
    d = open(path, "rb").read()
    if d[:8] != b"SSPCSHC1":
        raise SystemExit(f"{path}: not a shader cache file")
    pos = 64
    while pos + 24 <= len(d):
        magic, typ, size = struct.unpack_from("<IBxxxI", d, pos)
        if magic != REC_MAGIC or pos + 24 + size > len(d):
            break
        yield chr(typ), d[pos + 24: pos + 24 + size]
        pos += 24 + size


class Reader:
    def __init__(self, b):
        self.b, self.p = b, 0

    def take(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def u8(self):
        return self.take(1)[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def words(self):
        n = self.u32()
        return struct.unpack(f"<{n}I", self.take(4 * n))


def load(path):
    progs, modules, pipes = {}, [], []
    for typ, b in records(path):
        r = Reader(b)
        if typ == "P":
            ident = (r.u64(), r.u64(), r.u32(), r.u8())
            progs[ident] = True
        elif typ == "M":
            ident = (r.u64(), r.u64(), r.u32(), r.u8())
            layout = r.u8()
            r.u64()  # footer hash
            rt_size = r.u32()
            runtime = r.take(rt_size)
            start = (r.u32(), r.u32())
            r.take(64)  # user data
            key = r.words()
            r.u64()
            spirv = (r.u64(), r.u64())
            modules.append({"id": ident, "layout": layout, "runtime": runtime, "start": start, "key": key,
                            "spirv": spirv})
        elif typ == "L":
            compute, has_ps = r.u8(), r.u8()
            vs = (r.u64(), r.u64())
            ps = (r.u64(), r.u64())
            st_size = r.u32()
            state = r.take(st_size)
            rt_size = r.u32()
            fs = r.take(rt_size)
            n = r.u32()
            binds = r.take(8 * n)
            pipes.append({"compute": compute, "vs": vs, "ps": ps if has_ps else None, "state": state, "fs": fs,
                          "binds": binds})
    return progs, modules, pipes


def runtime_wo_rsrc(rt):
    # RuntimeParams: hw, sw, pad[2], rsrc1, rsrc2, ... - the program's own register counts are in RSRC1/RSRC2
    return rt[:4] + rt[12:]


def main(argv):
    scenes = {}
    out = None
    index = None
    i = 1
    while i < len(argv):
        if argv[i] == "--scene":
            name, path = argv[i + 1].split("=", 1)
            scenes[name] = load(path)
            i += 2
        elif argv[i] == "--out":
            out = argv[i + 1]
            i += 2
        elif argv[i] == "--index":
            index = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    t = []
    t.append("| сцена X | программ | известны по другим сценам | модулей (SPIR-V) | известны | конвейеров | известны (Exact) "
             "| новых: оба SPIR-V известны (новая комбинация) | новых: состояние+привязки встречались | новых модулей "
             "| их RuntimeParams встречались | без RSRC — встречались | ключ встречался | runtime без RSRC + ключ |")
    t.append("|" + "---|" * 14)
    desc_detail = []
    for name, (progs, mods, pipes) in scenes.items():
        rest = [v for k, v in scenes.items() if k != name]
        r_progs = set().union(*[set(p) for p, _, _ in rest]) if rest else set()
        r_spirv = set(m["spirv"] for _, ms, _ in rest for m in ms)
        r_runtime = set(m["runtime"] for _, ms, _ in rest for m in ms)
        r_runtime_wo = set(runtime_wo_rsrc(m["runtime"]) for _, ms, _ in rest for m in ms)
        r_keys = set(m["key"] for _, ms, _ in rest for m in ms if m["key"])
        r_rt_key = set((runtime_wo_rsrc(m["runtime"]), m["key"]) for _, ms, _ in rest for m in ms if m["key"])
        r_pipes = set((p["compute"], p["vs"], p["ps"], p["state"], p["fs"], p["binds"]) for _, _, ps in rest for p in ps)
        r_states = set((p["compute"], p["state"], p["fs"], p["binds"]) for _, _, ps in rest for p in ps)
        known_progs = sum(1 for p in progs if p in r_progs)
        spirv = set(m["spirv"] for m in mods)
        known_spirv = sum(1 for s in spirv if s in r_spirv)
        known_pipes = combo = state_seen = 0
        for p in pipes:
            key = (p["compute"], p["vs"], p["ps"], p["state"], p["fs"], p["binds"])
            if key in r_pipes:
                known_pipes += 1
                continue
            if p["vs"] in r_spirv and (p["ps"] is None or p["ps"] in r_spirv):
                combo += 1
            if (p["compute"], p["state"], p["fs"], p["binds"]) in r_states:
                state_seen += 1
        new_mods = [m for m in mods if m["spirv"] not in r_spirv]
        rt_seen = sum(1 for m in new_mods if m["runtime"] in r_runtime)
        rt_wo_seen = sum(1 for m in new_mods if runtime_wo_rsrc(m["runtime"]) in r_runtime_wo)
        key_seen = sum(1 for m in new_mods if m["key"] and m["key"] in r_keys)
        both_seen = sum(1 for m in new_mods if m["key"] and (runtime_wo_rsrc(m["runtime"]), m["key"]) in r_rt_key)
        # the key without its runtime part (the descriptors' formats and the fetch shader's form), keyed modules only
        rw = lambda m: len(m["runtime"]) // 4 + 2
        r_desc = set(m["key"][rw(m):] for _, ms, _ in rest for m in ms if m["key"])
        keyed_new = [m for m in new_mods if m["key"]]
        desc_seen = sum(1 for m in keyed_new if m["key"][rw(m):] in r_desc)
        desc_detail.append(f"{name}: new modules {len(new_mods)} = first modules of a layout {len(new_mods) - len(keyed_new)} + keyed "
                           f"{len(keyed_new)}; keyed whose descriptor part of the key was seen elsewhere: {desc_seen}")
        new_pipes = len(pipes) - known_pipes
        t.append(f"| {name} | {len(progs)} | {known_progs} | {len(spirv)} | {known_spirv} | {len(pipes)} | {known_pipes} | "
                 f"{combo} из {new_pipes} | {state_seen} из {new_pipes} | {len(new_mods)} | {rt_seen} | {rt_wo_seen} | "
                 f"{key_seen} | {both_seen} |")
    t.append("")
    t.extend("- " + x for x in desc_detail)
    if index:
        d = open(index, "rb").read()
        magic, ver, npacks, nprogs, nmembers = struct.unpack_from("<8sIIII", d, 0)
        pos = 40
        packs = []
        for _ in range(npacks):
            n = struct.unpack_from("<I", d, pos)[0]
            packs.append(d[pos + 4: pos + 4 + n].decode("utf-8", "replace"))
            pos += 4 + n
        game = {}
        for _ in range(nprogs):
            lo, hi, length, typ, fh, first, cnt = struct.unpack_from("<QQIIQII", d, pos)
            pos += 40
            game[(lo, hi, length)] = (typ, first, cnt)
        members = []
        for _ in range(nmembers):
            members.append(struct.unpack_from("<IIQ", d, pos))
            pos += 16
        t.append("\n| сцена | программ в кэше | найдены в файлах игры | тип футера -> стадия рантайма (HwStage) | "
                 "пакетов с её программами | из них пакетов уровня (a*.xpps) |")
        t.append("|---|---|---|---|---|---|")
        all_seen = set()
        for name, (progs, mods, pipes) in scenes.items():
            found = 0
            mapping = collections.Counter()
            pk = set()
            for (lo, hi, length, stage) in progs:
                g = game.get((lo, hi, length))
                if g:
                    found += 1
                    all_seen.add((lo, hi, length))
                    mapping[(g[0], stage)] += 1
                    for j in range(g[1], g[1] + g[2]):
                        pk.add(members[j][1])
            level = sum(1 for k in pk if packs[k].split("/")[-1].startswith("a"))
            t.append(f"| {name} | {len(progs)} | {found} | "
                     + ", ".join(f"тип {a}->{b}: {n}" for (a, b), n in sorted(mapping.items()))
                     + f" | {len(pk)} | {level} |")
        t.append(f"\nпрограмм в игре: {len(game)}; встречено хотя бы в одной сцене: {len(all_seen)} "
                 f"({100.0 * len(all_seen) / max(len(game), 1):.1f} %)")
    text = "\n".join(t) + "\n"
    if out:
        open(out, "w", encoding="utf-8").write(text)
    print(text)


if __name__ == "__main__":
    main(sys.argv)
