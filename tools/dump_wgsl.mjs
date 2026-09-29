// Writes the resolved WGSL prelude and every shader to build/wgsl/, exactly as
// the app hands them to createShaderModule (PRELUDE + source).
import { mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs'
const VS = process.env.VS ?? '/home/claude/videoskillet'
const out = process.argv[2] ?? '../build/wgsl'
mkdirSync(out, { recursive: true })
const { PRELUDE } = await import(`${VS}/src/core/gpu/prelude.ts`)
writeFileSync(`${out}/_prelude.wgsl`, PRELUDE)
const dir = `${VS}/src/core/gpu/shaders`
for (const f of readdirSync(dir).filter(f => f.endsWith('.wgsl'))) {
  writeFileSync(`${out}/${f}`, readFileSync(`${dir}/${f}`, 'utf8'))
}
console.log('prelude lines', PRELUDE.split('\n').length)
