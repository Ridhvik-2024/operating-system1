#!/usr/bin/env python3
"""microlm_gen.py — M7: build-time trainer for the AI-OS micro language model.

The micro-LM is the OS's own tiny language model: a smoothed naive-Bayes
intent classifier (bg / urgent / normal) plus a bigram sentence generator,
both trained on the embedded corpus below at BUILD time. The trained
weights are emitted as a plain C header (src/microlm_weights.h) so the
OS build needs nothing but a C compiler — no Python, no runtime training,
no network, fully deterministic.

This is deliberately very small (a few hundred parameters): it exists so
the booted OS can demonstrate AI-native language behaviour (classify a
request into a scheduling class, generate OS-flavoured status prose)
without a big external model. Bigger LLMs plug into the same cortex seam
(cortex llm) when needed.

Run from the repo root:  python3 tools/microlm_gen.py
The script self-verifies: every phrase in SELFTEST must classify correctly
or the weights are NOT written and the run fails.
"""
import sys
import math
from collections import Counter

# --------------------------------------------------------------------------
# Classifier corpus: three intent classes for scheduling.
#   bg      — background / idle-priority work
#   urgent  — interactive, do-it-now work
#   normal  — everything else (default class)
CORPUS = {
    "bg": [
        "run this in the background at idle priority",
        "run the nightly report in the background",
        "do it later when the system is idle",
        "no rush whenever you get a chance",
        "whenever the machine is quiet",
        "keep it at idle priority please",
        "run it slowly in the background",
        "defer this until the system is quiet",
        "overnight batch work is fine",
        "let it wait until the console is idle",
        "when the machine is idle tidy the notes",
        "background cleanup whenever convenient",
        "at idle priority please and thank you",
        "schedule it for later when quiet",
        "low priority background work",
        "take your time no rush at all",
        "in the background sometime tonight",
        "whenever the cpu is free run the audit",
        "let the agents chew on this at idle priority",
        "defer the audit until the system is quiet tonight",
        "no hurry run the numbers in the background",
        "background the summary job for later",
        "run the cleanup slowly at idle priority",
        "it can wait until the console is quiet",
        "whenever convenient organize the archive",
        "idle time work no rush",
        "let it soak in the background overnight",
        "when nothing else needs the cpu do this",
        "queue it as background work for tonight",
        "slowly does it at the lowest priority",
    ],
    "urgent": [
        "the console is frozen fix it asap",
        "respond immediately the user is waiting",
        "this is urgent stop everything and handle it now",
        "the shell is stuck help immediately",
        "fix this right now it is on fire",
        "emergency the system will not respond",
        "drop everything and answer me now",
        "the console will not answer hurry",
        "this cannot wait act immediately",
        "asap the demo starts in one minute",
        "hurry the user needs an answer right now",
        "stop the batch work the console needs you now",
        "critical failure respond at once",
        "immediately show me what is wrong",
        "now now now answer the console",
        "the user has been waiting for ages hurry up",
        "urgent the report is due this second",
        "answer immediately the screen is frozen",
        "this is on fire fix it now",
        "no time to waste respond at once",
        "the shell stopped responding help now",
        "fast fast the console is stuck",
        "act now the system is unhealthy",
        "i need this immediately not later",
        "drop the idle work and answer right now",
    ],
    "normal": [
        "summarize the workspace files",
        "read the notes and write a summary",
        "what files are in the notes folder",
        "calculate the token estimate for the corpus",
        "organize the archive folder",
        "explain how the scheduler treats agents",
        "analyze the metrics and draft a memo",
        "how many wakeups did the system count",
        "show me the kernel registry",
        "list the files in the workspace",
        "what does the semantic bus carry",
        "where do the memory tiers live",
        "read metrics and count the rows",
        "which tools are registered",
        "why does the jail sandbox the workers",
        "draft a memo about the milestones",
        "who watches the cgroup budgets",
        "research the logs and organize the findings",
        "count the lines in notes",
        "what is in the store journal",
        "describe the boot sequence of the os",
        "compare the episodic and semantic tiers",
        "clean the temporary folder",
        "audit the workspace layout",
        "how does the governor retune the budgets",
        "tell me about the intent classes",
        "generate a summary of the archive",
        "what happened during the last boot",
        "sort the notes by size",
        "show the boot journal entries",
    ],
}

# --------------------------------------------------------------------------
# Generator corpus: the OS's own voice (bigram training text, one sentence
# per line, no labels). Used by `lm gen`.
GEN_LINES = [
    "the governor watches wakeups and retunes the cpu budgets",
    "interactive work always runs ahead of batch tools",
    "background agents use the idle budget when the machine is quiet",
    "the semantic bus carries typed events between subsystems",
    "memory keeps the working episodic and semantic tiers apart",
    "tools run inside a jailed worker with a bounded budget",
    "selftests verify every subsystem before the console goes live",
    "wakeups per intent class show which workloads want the cpu",
    "the kernel bridge registers every process by intent class",
    "cpu budgets move between tools and agents as the load shifts",
    "the shell routes natural language through the intent classes",
    "a tiny language model lives inside the system services layer",
    "the scheduler sees intents not just nice numbers",
    "every tool call lands in the append only audit journal",
    "boot ends with a selftest of the whole system services layer",
    "the console is the face of the operating system",
    "agents spawn with their own memory tools and cortex",
    "recall ranks the relevant memories first",
    "consolidation promotes episodic facts to the semantic tier",
    "the audit journal makes every action answerable",
    "cgroup budgets bound the cost of machine work",
    "the intent registry lives in the kernel bridge module",
    "when the machine is quiet the agents soak up the idle budget",
    "the micro model classifies each request into a scheduling class",
    "big models can replace the micro model at the cortex seam",
    "the governor returns borrowed budgets when a class goes quiet",
    "natural language is a system interface not an application",
    "the operating system schedules by intent first and priority second",
]

SELFTEST = [
    ("run the nightly report in the background at idle priority", "bg"),
    ("no rush whenever the system is quiet", "bg"),
    ("do the cleanup later overnight", "bg"),
    ("keep that at idle priority please", "bg"),
    ("whenever you get a chance tidy the notes", "bg"),
    ("queue it as background work for tonight", "bg"),
    ("the console is frozen fix it asap", "urgent"),
    ("respond immediately the user is waiting", "urgent"),
    ("this is urgent stop everything now", "urgent"),
    ("the shell is stuck handle it right now", "urgent"),
    ("drop the idle work and answer right now", "urgent"),
    ("hurry the demo starts now", "urgent"),
    ("summarize the workspace files", "normal"),
    ("what files are in the notes folder", "normal"),
    ("calculate the token estimate", "normal"),
    ("explain how the scheduler treats agents", "normal"),
    ("read the metrics and draft a memo", "normal"),
    ("show me the kernel registry", "normal"),
]

END = 0xFFFF  # bigram terminator for the generator


def tokens(line):
    out, cur = [], []
    for ch in line.lower():
        if ch.isalnum() or ch == "'":
            cur.append(ch)
        elif cur:
            out.append("".join(cur))
            cur = []
    if cur:
        out.append("".join(cur))
    return out


def main():
    classes = ["bg", "urgent", "normal"]

    # ---------------- vocab + classifier counts (unigrams) ----------------
    cls_words = set()
    for ws in CORPUS.values():
        for line in ws:
            cls_words.update(tokens(line))
    words = sorted(cls_words)                 # sorted: binary searchable
    idx = {w: i for i, w in enumerate(words)}
    V = len(words)

    cnt = [[0] * V for _ in classes]
    tot = [0] * len(classes)
    for ci, cname in enumerate(classes):
        for line in CORPUS[cname]:
            for w in tokens(line):
                cnt[ci][idx[w]] += 1
                tot[ci] += 1

    def classify(text):
        toks = tokens(text)
        scores = []
        for ci in range(len(classes)):
            s = 0.0
            for w in toks:
                i = idx.get(w)
                if i is not None:
                    s += math.log((cnt[ci][i] + 1.0) / (tot[ci] + V))
            scores.append(s)
        best = max(range(len(classes)), key=lambda c: (scores[c], -c))
        mx = max(scores)
        exps = [math.exp(s - mx) for s in scores]
        conf = exps[best] / sum(exps)
        return best, conf

    print("self-verify (all must be correct before weights are written):")
    fails = 0
    for text, want in SELFTEST:
        got, conf = classify(text)
        ok = classes[got] == want
        if not ok:
            fails += 1
        print(f"  [{'ok ' if ok else 'FAIL'}] {want:<7} -> {classes[got]:<7} "
              f"({conf:.2f})  {text}")
    if fails:
        print(f"SELFTEST FAILED: {fails} misclassified — weights NOT written")
        return 1

    # ---------------- generator bigrams (extends vocab with gen words) ---
    # Keep the WHOLE vocab sorted (classifier + generator words) so the C
    # side can binary-search it: sort once here and remap every index.
    cls_idx = dict(idx)                     # classifier-word indices
    cls_cnt = [row[:] for row in cnt]       # counts in cls_idx order
    all_words = sorted(set(words) | {w for l in GEN_LINES for w in tokens(l)})
    idx = {w: i for i, w in enumerate(all_words)}
    words = all_words
    V = len(words)

    # remap classifier counts to the new (sorted) order
    cnt = [[0] * V for _ in classes]
    for w, old_i in cls_idx.items():
        new_i = idx[w]
        for ci in range(len(classes)):
            cnt[ci][new_i] = cls_cnt[ci][old_i]

    starts = [idx[tokens(l)[0]] for l in GEN_LINES if tokens(l)]
    bigrams = []
    for line in GEN_LINES:
        tk = tokens(line)
        for a, b in zip(tk, tk[1:]):
            bigrams.append((idx[a], idx[b]))
        bigrams.append((idx[tk[-1]], END))
    bigrams.sort()

    # ---------------- emit C header ----------------
    def carr(vals, per=15):
        out, line = [], []
        for v in vals:
            line.append(str(v))
            if len(line) == per:
                out.append("    " + ", ".join(line) + ",")
                line = []
        if line:
            out.append("    " + ", ".join(line) + ",")
        return "\n".join(out)

    sarr = "\n".join(f'    "{s}",' for s in words)

    hdr = f"""\
/* microlm_weights.h — AUTO-GENERATED by tools/microlm_gen.py — DO NOT EDIT.
 *
 * Trained weights of the AI-OS micro language model (M7):
 *   - naive-Bayes intent classifier over {len(cls_words)} corpus words
 *     (classes: bg, urgent, normal), Laplace-smoothed unigrams
 *   - bigram sentence generator over the OS voice corpus
 * Deterministic: identical input -> identical output; no runtime training.
 */
#ifndef AIOS_MICROLM_WEIGHTS_H
#define AIOS_MICROLM_WEIGHTS_H

#define MICROLM_WEIGHTS_VERSION "m7.1"
#define MICROLM_NWORDS   {V}
#define MICROLM_NCLASSES 3
#define MICROLM_NSTARTS  {len(starts)}
#define MICROLM_NBIGRAMS {len(bigrams)}

/* vocab (sorted ascending — binary searchable) */
static const char *const MICROLM_WORDS[MICROLM_NWORDS] = {{
{sarr}
}};

/* per-class unigram counts, row-major: class * MICROLM_NWORDS + word */
static const unsigned short MICROLM_CLS_CNT[MICROLM_NCLASSES * MICROLM_NWORDS] = {{
{carr([c for ci in range(3) for c in cnt[ci]])}
}};

/* per-class token totals */
static const unsigned int MICROLM_CLS_TOT[MICROLM_NCLASSES] = {{
    {tot[0]}, {tot[1]}, {tot[2]}
}};

/* sentence-start word indices for the generator */
static const unsigned short MICROLM_STARTS[MICROLM_NSTARTS] = {{
{carr(starts)}
}};

/* bigram transitions (from, to); to == 0xFFFF ends the sentence */
static const unsigned short MICROLM_BIGRAMS[MICROLM_NBIGRAMS * 2] = {{
{carr([x for pr in bigrams for x in pr])}
}};

#endif /* AIOS_MICROLM_WEIGHTS_H */
"""
    with open("src/microlm_weights.h", "w") as f:
        f.write(hdr)
    print(f"wrote src/microlm_weights.h: vocab={V} (classifier {len(cls_words)}) "
          f"starts={len(starts)} bigrams={len(bigrams)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
