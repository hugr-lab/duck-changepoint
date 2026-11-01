# Costs and penalties

Notation: the series is x₁..x_N. The segment (a, b] holds the points a+1..b, has length n = b − a,
sum S and mean x̄ = S/n. In code it is the half-open index range `[a, b)` over the 0-based array.

## Pruning condition

PELT discards a candidate last change s at time t when F(s) + C(s, t) + K > F(t). Killick et al.
(2012), Theorem 3.1, requires a constant K with

    C(a, s) + C(s, b) + K ≤ C(a, b)   for all a < s < b.

Every cost below is the minimum over segment parameters of a sum of per-point terms. Splitting a
segment lets each half choose its own parameters, which can only lower the total, so K = 0 works
for all four.

With MBIC each segment also pays ln(n). Because ln x + ln y − ln(x + y) ≤ ln(min(x, y)) ≤ ln N, the
modified cost satisfies the condition with K = −ln N. `cpt_pelt` uses that value: a smaller K prunes
less but never wrongly.

## l2

    C = Σ (xᵢ − x̄)² = (Σ xᵢ² − S²/n)

This is −2 log-likelihood of a Gaussian with unknown mean and unit variance, up to a constant.
Two prefix arrays hold Σx and Σx² of the series centred on its global mean. Centring keeps
Σx² − S²/n well conditioned when the mean is large compared with the noise; Nile, for example, is
about 900 ± 170. Results are clamped at 0 against rounding.

## normal

    C = n · ln σ̂²,   σ̂² = max(Σ (xᵢ − x̄)² / n, 1e-11)

This is −2 log-likelihood with both mean and variance free, minus n(ln 2π + 1). That constant is
linear in n, so it adds the same amount to every segmentation and does not change the optimum.
changepoint's `mll.meanvar` keeps the constant and uses the same 1e-11 floor. Segments need at least
2 points: the MLE variance of one point is 0.

The floor is absolute, so rescale series whose variance is below about 1e-9. Near the floor,
ln(max(v, ε)) is not concave, and the K = 0 bound can fail by a tiny amount for segments whose
variance is just above ε. For data on a normal scale this does not happen.

## poisson

    C = −2 (S ln(S/n) − S),   C = 0 when S = 0

This is −2 log-likelihood of Poisson counts with rate S/n, without the Σ ln xᵢ! term, which does not
depend on the segmentation. The input must be non-negative integers; anything else raises an error.

## l1

    C = Σ |xᵢ − m|,   m = a median of the segment

Any value between the two middle order statistics gives the same cost, so the lower median is used.
The wavelet matrix in `src/core/cost_l1.c` is built over the ranks of x (ties broken by position, so
the ranks are a permutation). For [a, b) and k = ⌊(n − 1)/2⌋, one O(log N) descent returns the k-th
smallest value m and the sum `below` of the k smaller values. Then

    C = (m·k − below) + ((S − below − m) − m·(n − k − 1))

Memory: ⌈log₂ N⌉ levels, each with a uint32 rank array and a double prefix-sum array.

## Penalties

p is the number of parameters per segment: 1 for l2, poisson and l1, and 2 for normal.

| name | β per change point | per segment |
|---|---|---|
| bic | (p + 1) ln N | |
| mbic | (p + 2) ln N | ln nᵢ |
| aic | 2 (p + 1) | |

These are the values in R changepoint's `penalty_decision`, where the location of a change counts as
one parameter. Note that this is p + 1, not p: with p · ln N, the l2 penalty on N = 1000 points of
unit-variance noise is 6.9, and the maximum gain from a spurious split exceeds that often enough to
add false change points. 2 ln N = 13.8 does not.

PELT minimises Σ C + β · (#segments − 1); internally F(0) = −β. ruptures adds β per segment, which
shifts the objective by a constant and gives the same optimum.
