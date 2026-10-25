// Checks a parsed benchmark report: its format's schema, the plugin fields
// against the schemas in the same report, and plausibility against the rows
// (docs/spec/benchmark-v1.md, section 9, check 4).

// Compiled from schemas/ by scripts/compile-schemas.js.
import validateFormat1 from "./generated/report-format-1.js";

// Every accepted format and its checks. A format stays here until it is
// retired explicitly; a new one is added before a Manager that sends it ships.
const FORMATS = new Map([[1, { validate: validateFormat1, check: checkFormat1 }]]);

export function acceptedFormats() {
  return [...FORMATS.keys()];
}

// Returns { ok: true, format } or { ok: false, format, message }. `format` is
// the report's format number when it is one, otherwise null.
export function checkReport(report, limits) {
  const format = report !== null && typeof report === "object" && Number.isInteger(report.format) ? report.format : null;
  if (format === null || !FORMATS.has(format)) {
    return {
      ok: false,
      format,
      message: `This server does not accept report format ${format ?? "(none)"}. Update Anvil.`,
    };
  }
  const { validate, check } = FORMATS.get(format);
  if (!validate(report)) {
    return { ok: false, format, message: `The report does not match format ${format}: ${describe(validate.errors)}` };
  }
  const problem = check(report, limits);
  if (problem) {
    return { ok: false, format, message: `The report is not plausible: ${problem}` };
  }
  return { ok: true, format };
}

// The deepest error is the most specific one; the enclosing if/then keywords
// are reported too.
function describe(errors) {
  let best = errors[0];
  for (const e of errors) {
    if (e.instancePath.length > best.instancePath.length) best = e;
  }
  const extra = best.params?.additionalProperty ?? best.params?.allowedValues?.join(", ");
  return `${best.instancePath || "/"} ${best.message}${extra ? ` (${extra})` : ""}`.slice(0, 300);
}

function checkFormat1(report, limits) {
  return checkRows(report, limits) ?? checkPluginFields(report) ?? checkResult(report);
}

function checkRows(report, limits) {
  for (const [i, start] of report.starts.entries()) {
    if (start.position !== i + 1) return `starts[${i}] has position ${start.position}, expected ${i + 1}`;
    if (start.time_ms !== null && (start.time_ms < limits.minStartMs || start.time_ms > limits.maxStartMs)) {
      return `start ${start.position} took ${start.time_ms} ms, outside ${limits.minStartMs}-${limits.maxStartMs} ms`;
    }
  }
  return null;
}

// Plugin fields are checked generically: every value must be declared in
// plugin_schemas and match its kind; null means never set.
function checkPluginFields(report) {
  for (const start of report.starts) {
    for (const [id, entry] of Object.entries(start.plugins)) {
      if (entry.state !== "loaded" || !entry.participant) continue;
      const schema = report.plugin_schemas[id];
      if (!schema) return `plugin ${id} is a benchmark participant in start ${start.position} but has no field schema`;
      for (const [name, value] of Object.entries(entry.fields)) {
        const field = schema[name];
        if (!field) return `field ${name} of plugin ${id} in start ${start.position} is not declared`;
        if (value !== null && !matchesKind(field, value)) {
          return `field ${name} of plugin ${id} in start ${start.position} is not a valid ${field.kind}`;
        }
      }
    }
  }
  return null;
}

function matchesKind(field, value) {
  switch (field.kind) {
    case "int":
      return Number.isInteger(value);
    case "number":
      return typeof value === "number" && Number.isFinite(value);
    case "enum":
      return typeof value === "string" && field.options.includes(value);
    case "flag":
      return typeof value === "boolean";
  }
  return false;
}

// The derived result must be the one the rows give: the warm group per arm and
// the difference of the medians.
function checkResult(report) {
  const { warm, difference } = report.result;
  for (const arm of ["without", "with"]) {
    const times = report.starts
      .filter((s) => s.valid && s.arm === arm && s.cold_start.class === "warm")
      .map((s) => s.time_ms)
      .sort((a, b) => a - b);
    const summary = warm[arm];
    const reported = [...summary.times_ms].sort((a, b) => a - b);
    if (summary.n !== times.length || reported.join() !== times.join()) {
      return `the warm starts ${arm} the plugin do not match the rows`;
    }
    if (times.length === 0) {
      if (summary.median_ms !== null || summary.min_ms !== null || summary.max_ms !== null) {
        return `the warm group ${arm} the plugin is empty but has a median or range`;
      }
    } else if (
      summary.median_ms === null ||
      Math.abs(summary.median_ms - median(times)) > 0.5 ||
      summary.min_ms !== times[0] ||
      summary.max_ms !== times.at(-1)
    ) {
      return `the warm median or range ${arm} the plugin does not match the rows`;
    }
  }
  const both = warm.without.n > 0 && warm.with.n > 0;
  if (!both) return difference === null ? null : "a difference is given without warm starts in both arms";
  if (difference === null) return "the difference is missing";
  const ms = warm.with.median_ms - warm.without.median_ms;
  if (Math.abs(difference.ms - ms) > 1) return "the difference does not match the medians";
  if (Math.abs(difference.percent - (ms / warm.without.median_ms) * 100) > 0.05) {
    return "the difference in percent does not match the medians";
  }
  const overlap = warm.with.min_ms <= warm.without.max_ms && warm.without.min_ms <= warm.with.max_ms;
  if (difference.ranges_overlap !== overlap) return "ranges_overlap does not match the ranges";
  return null;
}

function median(sorted) {
  const mid = sorted.length >> 1;
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}
