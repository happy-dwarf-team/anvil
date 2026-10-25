// Compiles every report format's JSON Schema into standalone validation code,
// since a Worker cannot generate code at run time and the Workers Free plan
// allows 10 ms of CPU per request. Runs before the tests and before deploy.

import { mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import Ajv2020 from "ajv/dist/2020.js";
import standaloneCode from "ajv/dist/standalone/index.js";

const root = new URL("..", import.meta.url);
const out = new URL("src/generated/", root);
mkdirSync(out, { recursive: true });

for (const file of readdirSync(new URL("schemas/", root))) {
  const match = /^report-format-(\d+)\.schema\.json$/.exec(file);
  if (!match) continue;
  const schema = JSON.parse(readFileSync(new URL(`schemas/${file}`, root), "utf8"));
  const ajv = new Ajv2020({ code: { source: true, esm: true }, strict: true, strictTypes: false, strictRequired: false });
  // Ajv's ESM output still loads its runtime helpers with require(); turn them
  // into imports. The helpers are CommonJS modules with a default export.
  const code = standaloneCode(ajv, ajv.compile(schema)).replace(
    /const (\w+) = require\("([^"]+)"\)\.default/g,
    (_, name, module) => `import ${name}$ from "${module}.js";const ${name} = ${name}$.default ?? ${name}$`,
  );
  if (code.includes("require(")) throw new Error(`${file}: the generated code still calls require()`);
  writeFileSync(new URL(`report-format-${match[1]}.js`, out), `// Generated from schemas/${file} by scripts/compile-schemas.js.\n${code}`);
}
