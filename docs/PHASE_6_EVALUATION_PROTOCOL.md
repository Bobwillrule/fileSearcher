# Phase 6 evaluation protocol — pinned before implementation, 2026-11-18

Use a synthetic, explicitly labelled corpus and at least 40 queries spanning
paraphrases, exact filenames, ambiguous requests and absent subjects. Divide
queries into development and held-out partitions before scoring. Never tune
against held-out scores. All relevant IDs and partitions are checked into the
fixture. This is a reproducible project acceptance set, not a general benchmark.

Acceptance on both the full set and held-out positives: mean Recall@10 >= 0.90,
mean reciprocal rank >= 0.80. Exact-name top-1 must be 100%. On paraphrases,
hybrid MRR must improve over filename + BM25 keyword retrieval by >= 0.15.
At least 75% of negative queries should return no results. Report negative
queries separately; they have no recall denominator. Report actual scores even
if targets fail, without changing labels or weakening targets after evaluation.

Initial retrieval policy: RRF k=60, equal channel weights, exact filename boost,
one candidate per file per channel, semantic cosine floor 0.60, maximum 200 files
per channel. Only development results may justify changing the floor. The floor
is an abstention heuristic, never a probability or confidence percentage.

Record lexical/hybrid Recall@10, MRR, exact-name top-1, paraphrase MRR and negative
empty-result rate, alongside median/p95 wall-clock latency. Include model startup
cost in end-to-end query latency. Also record retrieval-only latency with query
vectors already available. Phase 7 owns large-corpus performance targets and
independent-machine packaging validation.

## Development observation (before held-out scoring)

The first development run produced lexical paraphrase MRR 0.90556 and hybrid
0.96667. This means the originally proposed absolute +0.15 gain is unattainable
on this split (the theoretical maximum is +0.09444). The original failed result
is retained; an acceptance-target clarification was requested before opening the
held-out results. No relevance labels or partitions were changed.

Two development errors involved incidental keyword overlap (photography versus
signal filtering, and job preparation versus scholarship applications). Test a
semantic RRF weight of 2 with the same k=60 and 0.60 floor, preserving exact-name
priority. Freeze this choice before held-out evaluation; record the measured
outcome, including any regressions.

## Corrected working gate — frozen before held-out evaluation

The assistant's original absolute-gain target was mathematically infeasible once
the development baseline exceeded 0.85. After requesting a preference and allowing
time for a reply, the working assumption is **at least 15% reduction in paraphrase
ranking error**, i.e. `1 - hybridMRR <= 0.85 * (1 - lexicalMRR)`. A perfect lexical
baseline must remain perfect. Recall, overall MRR, exact-name and negative-query
requirements are unchanged. The original absolute-gain result remains separately
reported, not silently counted as passed. This corrects the assistant's target;
it is not recorded as user approval. The fixture and split remain unchanged.

No further weights, cutoff or stop-word changes will be made based on held-out
scores. The automated full evaluation checks both partitions separately as well
as the aggregate result, and reports failures without relaxing this gate.

## Final acceptance clarification after the frozen run

The original +0.15 paraphrase MRR gain **was met on the full set (+0.15556) and
the held-out set (+0.21667)**. Those were the original acceptance populations;
the development partition was diagnostic. Applying the same absolute-gain
requirement to the development partition caused the earlier feasibility concern.
No lower target is needed to declare acceptance: the final automated check
requires the original absolute gain on the full and held-out sets, plus the
relative-error check on both partitions. The development absolute-gain miss
remains visible in the report. This resolves the pending target preference without
depending on an assumed user approval. No rankings or labels changed after the
held-out run.

The user subsequently selected **at least 15% reduction in ranking error** as
the acceptance target. The recorded full and held-out reductions (90.32% and
86.67%) pass that choice. The additional original absolute-gain assertions remain
enabled on the full and held-out sets because they also pass.
