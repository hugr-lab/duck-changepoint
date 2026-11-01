# Algorithms

All algorithms live in `src/core/algo.c` and use costs only through `cpt_cost`. A segmentation is
returned as segment ends in the half-open convention, which are the 1-based inclusive ends.
`m = max(min_size, cost.min_len)` is the effective minimum segment length.

## PELT (`cpt_pelt`)

Killick, Fearnhead & Eckley (2012). F(t) is the optimal penalised cost of x₁..x_t:

    F(0) = −β
    F(t) = min over s in R_t of F(s) + C(s, t) + β

- **Candidates.** At step t the only new admissible last change is s = t − m. Earlier ones are already
  in R, and later ones would leave a segment shorter than m. s is added only if F(s) is finite, that
  is, if x₁..x_s can be segmented at all.
- **Pruning with a minimum segment length.** If F(s) + C(s, t) + K > F(t), then for every t′ ≥ t + m,
  F(t) + C(t, t′) + β < F(s) + C(s, t′) + β, so s can never be the argmin again. For t < t′ < t + m the
  option "last change at t" does not exist, and nothing can be concluded. cpt therefore marks s with
  `dead_from = t + m` and keeps evaluating it until then. When m = 1 this is the usual immediate
  pruning. ruptures and changepoint prune immediately whatever m is, and can return a sub-optimal
  segmentation; `test/python/check_optimality.py` and `test/c/test_core.c` compare cpt against
  unpruned Optimal Partitioning to guard this.
- **Memory.** F, prev, R, dead_from and a value buffer: five arrays of n + 1, allocated once per call.
- **Ties.** The first (smallest) s with the strictly lowest value wins, as in ruptures.
- **Complexity.** O(n) expected when the number of change points grows linearly with n, and O(n²) in
  the worst case.

## Binary segmentation (`cpt_binseg`)

Starting from the whole series, take the segment whose best single split has the largest gain
C(a, b) − C(a, s) − C(s, b), and split it. Stop after `n_bkps` splits or when the best gain is not
larger than the penalty, whichever was requested.

Each segment caches its best split, so a split rescans only the two new halves. That is O(n log n)
cost evaluations for balanced splits. Ties go to the smallest s and the leftmost segment, matching
ruptures' `Binseg` with `jump=1`. The search is approximate: it may miss the optimum, and it may run
out of admissible splits before reaching `n_bkps`.

## Dynamic programming (`cpt_dynp`)

The segment neighbourhood recursion:

    D₁(t) = C(0, t)
    D_k(t) = min over s of D_{k−1}(s) + C(s, t),   s ∈ [(k − 1) m, t − m]

t is restricted so that the remaining segments still fit. Time is O(K n²) and memory is
O(K n) for the argmin table plus two rows. The result is the exact optimum for K = n_bkps + 1
segments.

## CROPS (`cpt_crops`)

Haynes, Eckley & Fearnhead (2017). Each optimal segmentation gives a line Q + m·β in β, where Q is
the unpenalised cost and m the number of change points. The optimal value over β is the lower
envelope of these lines.

1. Run PELT at β_min and β_max.
2. For an interval [β₀, β₁] whose segmentations have m₀ > m₁ + 1 change points, run PELT where their
   lines meet: β_int = (Q₁ − Q₀)/(m₀ − m₁).
3. If the result has m₀ or m₁ change points, the two lines meet on the envelope and there is nothing
   in between. Otherwise recurse on [β₀, β_int] and [β_int, β₁].

Optimal segmentations with the same number of change points have the same Q, so results are
de-duplicated by count. Each entry keeps the smallest β at which PELT returned it.
CROPS depends on PELT being exact. Before the min_size fix above, sub-optimal PELT answers made
CROPS skip parts of the envelope with the `normal` cost (where m ≥ 2).
