// Static leak audit of src/ with Joern (https://joern.io).
//
//   make joern_leaks          (builds build/joern/cpg.bin with joern-parse, then runs this script)
//
// For every heap allocation (malloc/calloc/realloc, or the cpt_ wrappers in cpt_alloc.h) and every DuckDB handle creation
// (duckdb_create_*), check that the assigned value is
//   - released in the same function (free / cpt_*_free / cpt_*_destroy / duckdb_destroy_*) or handed to
//     DuckDB together with a delete callback (duckdb_*_set_bind_data / set_extra_info), or
//   - returned to the caller, directly or through an output parameter (`*out = p`), or
//   - stored in a struct field that some function releases (e.g. out->pos freed by cpt_bkps_free).
// Anything else is reported as UNRELEASED. This is a per-function audit, not path-sensitive:
// early returns on error paths are covered by the dynamic checks (make leak_check).

import io.shiftleft.semanticcpg.language.*

// Releases, plus hand-overs to DuckDB, which frees the pointer through the delete callback it is given
// (e.g. duckdb_scalar_function_set_bind_data(info, data, free)).
val releaseCalls =
  "free|cpt_.*_free|cpt_cost_destroy|cpt_crops_free|duckdb_destroy_.*|duckdb_.*_set_bind_data|duckdb_.*_set_extra_info"

def stripRef(code: String): String = code.stripPrefix("&").trim

// Field name of a target like "c->s1", "out->pos", "set.items", "c->rank0[l]" (-> "s1", "pos", ...)
def fieldOf(target: String): Option[String] =
  val t = target.replaceAll("\\[.*\\]", "").trim
  if t.contains("->") then Some(t.split("->").last.trim)
  else if t.contains(".") then Some(t.split("\\.").last.trim)
  else None

@main def main(cpgFile: String) = {
  importCpg(cpgFile)

  // every argument released anywhere, e.g. "c->s1", "&type", "c->rank0[l]"
  val releasedAnywhere = cpg.call.name(releaseCalls).argument.code.l.map(stripRef)
  val releasedFields = releasedAnywhere.flatMap(fieldOf).toSet

  def audit(kind: String, allocRegex: String): List[(String, String, Int, String, String)] =
    cpg.assignment
      .filter(_.source.ast.isCall.name(allocRegex).nonEmpty)
      .l
      .map { a =>
        val m = a.method
        val target = a.target.code.replaceAll("^\\*", "").trim
        val callee = a.source.ast.isCall.name(allocRegex).name.headOption.getOrElse("?")
        val released = m.ast.isCall.name(releaseCalls).argument.code.l.map(stripRef).contains(target)
        // returned, directly or as an output parameter (`*out = alloc(...)`)
        val returned = m.ast.isReturn.ast.isIdentifier.name.l.contains(target) || a.target.code.startsWith("*")
        val fieldOwned = fieldOf(target).exists(releasedFields.contains)
        // copied into a released local or field, e.g. the realloc idiom `tmp = realloc(p, n); p = tmp;`
        val releasedInMethod = m.ast.isCall.name(releaseCalls).argument.code.l.map(stripRef).toSet
        // or handed to the caller through an output parameter: `*out = p`
        val aliased = m.assignment.l.exists { b =>
          b.source.code == target &&
            (releasedInMethod.contains(b.target.code) || fieldOf(b.target.code).exists(releasedFields.contains) ||
              b.target.code.startsWith("*"))
        }
        val verdict =
          if released then "released"
          else if returned then "returned"
          else if fieldOwned || aliased then "owned-by-struct"
          else "UNRELEASED"
        (kind, m.name, a.lineNumber.getOrElse(-1), s"$target = $callee(...)", verdict)
      }

  val rows = audit("heap", "(cpt_)?(malloc|calloc|realloc)") ++ audit("duckdb", "duckdb_create_.*")
  println(f"${"kind"}%-7s ${"function"}%-28s ${"line"}%5s  ${"allocation"}%-52s verdict")
  rows.sortBy(r => (r._2, r._3)).foreach { case (k, f, l, what, v) =>
    println(f"$k%-7s $f%-28s $l%5d  ${what.take(52)}%-52s $v")
  }

  // Functions that return an owned pointer (directly, or by returning another allocator's result):
  // every call site must release or pass on the result.
  var returningAllocators = rows.filter(_._5 == "returned").map(_._2).toSet
  var grew = true
  while grew do
    val more = cpg.method
      .filter(_.ast.isReturn.ast.isCall.name(returningAllocators.mkString("|")).nonEmpty)
      .name.toSet.filterNot(_.startsWith("<")) -- returningAllocators
    grew = more.nonEmpty
    returningAllocators ++= more
  val callerRows = cpg.assignment
    .filter(_.source.ast.isCall.name(returningAllocators.mkString("|")).nonEmpty)
    .l
    .map { a =>
      val m = a.method
      val target = a.target.code.trim
      val released = m.ast.isCall.name(releaseCalls).argument.code.l.map(stripRef).contains(target)
      val returned = m.ast.isReturn.ast.isIdentifier.name.l.contains(target)
      val fieldOwned = fieldOf(target).exists(releasedFields.contains)
      (m.name, a.lineNumber.getOrElse(-1), a.code.take(60),
        if released then "released" else if returned then "returned" else if fieldOwned then "owned-by-struct" else "UNRELEASED")
    }
  println()
  println(s"call sites of allocating functions (${returningAllocators.toList.sorted.mkString(", ")}):")
  callerRows.sortBy(r => (r._1, r._2)).foreach { case (f, l, c, v) => println(f"  $f%-28s $l%5d  $c%-60s $v") }

  val bad = rows.count(_._5 == "UNRELEASED") + callerRows.count(_._4 == "UNRELEASED")
  println()
  println(s"${rows.size} allocations, ${callerRows.size} call sites of allocating functions, $bad unreleased")
  if bad > 0 then System.exit(1)
}
