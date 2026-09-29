// Translate every videoskillet shader (prelude + source, as the app compiles
// them) to GLSL, writing <out>/<shader>.<entry>.glsl plus bindings.json.
import { mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs'
import { translate } from './wgsl2glsl.mjs'
const inDir = process.argv[2] ?? '../build/wgsl'
const outDir = process.argv[3] ?? '../build/glsl'
mkdirSync(outDir, { recursive: true })
const prelude = readFileSync(`${inDir}/_prelude.wgsl`, 'utf8')
const table = {}
let fails = 0
for (const f of readdirSync(inDir).filter(f => f.endsWith('.wgsl') && !f.startsWith('_')).sort()) {
  if (f === 'blit_ext.wgsl') continue // browser-only external video import
  const name = f.replace('.wgsl', '')
  try {
    const res = translate(prelude + '\n' + readFileSync(`${inDir}/${f}`, 'utf8'))
    for (const [entry, r] of Object.entries(res)) {
      writeFileSync(`${outDir}/${name}.${entry}.glsl`, r.src + '\n')
      table[`${name}.${entry}`] = { stage: r.stage, bindings: r.bindings, workgroupSize: r.workgroupSize }
    }
  } catch (err) {
    fails++
    console.log(`FAIL ${f}: ${err.message}`)
  }
}
writeFileSync(`${outDir}/bindings.json`, JSON.stringify(table, null, 1))
console.log(`translated ${Object.keys(table).length} entry points, ${fails} failures`)
