#!/usr/bin/env Rscript
# Export reference datasets and changepoint (R) results to test/data/. Not run in CI; the CSV and
# JSON outputs are committed.
#
#   Rscript tools/export_reference.R              (needs install.packages(c("changepoint", "jsonlite")))
#   make reference-data                           (same, inside a disposable r-base container)
#
# Every expected_*.json holds, per result, the changepoint call, its penalty value and the change
# points both in changepoint's convention (cpts: last index of each segment except the final one)
# and in cpt's convention (bkps: 1-based segment ends including n).

suppressPackageStartupMessages({
  library(changepoint)
  library(jsonlite)
})

out_dir <- file.path("test", "data")
dir.create(out_dir, showWarnings = FALSE, recursive = TRUE)

run <- function(fun, x, method, penalty, ...) {
  res <- switch(fun,
    cpt.mean = cpt.mean(x, method = method, penalty = penalty, test.stat = "Normal", ...),
    cpt.meanvar = cpt.meanvar(x, method = method, penalty = penalty, test.stat = "Normal", ...),
    stop("unknown function ", fun)
  )
  cp <- as.integer(cpts(res))
  list(
    "function" = fun,
    method = method,
    penalty = penalty,
    pen_value = pen.value(res),
    minseglen = minseglen(res),
    cpts = I(cp),
    bkps = I(c(cp, length(x)))
  )
}

write_expected <- function(name, column, x, results) {
  doc <- list(
    dataset = name,
    column = column,
    n = length(x),
    changepoint_version = as.character(packageVersion("changepoint")),
    r_version = R.version.string,
    results = results
  )
  write_json(doc, file.path(out_dir, paste0("expected_", name, ".json")),
             auto_unbox = TRUE, digits = NA, pretty = TRUE)
}

# --- Nile: annual flow at Aswan 1871-1970 (datasets::Nile) -----------------------------------
nile <- as.numeric(datasets::Nile)
write.csv(data.frame(year = 1871:1970, flow = nile), file.path(out_dir, "nile.csv"), row.names = FALSE)
write_expected("nile", "flow", nile, list(
  run("cpt.meanvar", nile, "PELT", "BIC"),
  run("cpt.meanvar", nile, "PELT", "MBIC"),
  # cpt.mean's Normal cost assumes unit variance: scale by the sd first, as the vignette advises
  run("cpt.mean", nile / sd(nile), "PELT", "BIC"),
  run("cpt.mean", nile / sd(nile), "PELT", "MBIC")
))

# --- Lai2005fig4: aCGH, GBM31 (column 5), as in the changepoint vignette ---------------------
data("Lai2005fig4", package = "changepoint")
write.csv(Lai2005fig4, file.path(out_dir, "lai2005fig4.csv"), row.names = FALSE)
gbm31 <- as.numeric(Lai2005fig4[, 5])
write_expected("lai2005fig4", colnames(Lai2005fig4)[5], gbm31, list(
  run("cpt.mean", gbm31, "PELT", "MBIC"),
  run("cpt.mean", gbm31, "PELT", "BIC")
))

# --- wave.c44137: significant wave height, buoy C44137 ---------------------------------------
data("wave.c44137", package = "changepoint")
wave <- as.numeric(wave.c44137)
write.csv(data.frame(t = seq_along(wave), height = wave), file.path(out_dir, "wave_c44137.csv"),
          row.names = FALSE)
write_expected("wave_c44137", "height", wave, list(
  run("cpt.meanvar", wave, "PELT", "MBIC"),
  run("cpt.meanvar", wave, "PELT", "BIC")
))

# --- ftse100: daily returns -------------------------------------------------------------------
data("ftse100", package = "changepoint")
write.csv(ftse100, file.path(out_dir, "ftse100.csv"), row.names = FALSE)
ftse <- as.numeric(ftse100[, 2])
write_expected("ftse100", colnames(ftse100)[2], ftse, list(
  run("cpt.meanvar", ftse, "PELT", "MBIC"),
  run("cpt.meanvar", ftse, "PELT", "BIC")
))

cat("wrote reference data to", out_dir, "\n")
