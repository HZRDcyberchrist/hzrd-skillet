// Node ESM resolve hook: lets plain `node` import the videoskillet TypeScript
// sources, which use extensionless relative imports and `?raw` shader imports.
import { existsSync, readFileSync } from 'node:fs'
import { fileURLToPath, pathToFileURL } from 'node:url'
import path from 'node:path'
export async function resolve(spec, ctx, next) {
  if (spec.endsWith('?raw')) {
    const p = path.resolve(path.dirname(fileURLToPath(ctx.parentURL)), spec.slice(0, -4))
    return { url: pathToFileURL(p).href + '?raw', shortCircuit: true }
  }
  if ((spec.startsWith('.') || spec.startsWith('/')) && ctx.parentURL) {
    const base = path.resolve(path.dirname(fileURLToPath(ctx.parentURL)), spec)
    for (const ext of ['', '.ts', '.tsx', '/index.ts']) {
      if (existsSync(base + ext) && (ext !== '' || !existsSync(base + '/'))) {
        if (ext === '' && !/\.[cm]?[jt]sx?$/.test(base)) continue
        return { url: pathToFileURL(base + ext).href, shortCircuit: true }
      }
    }
  }
  return next(spec, ctx)
}
export async function load(url, ctx, next) {
  if (url.endsWith('?raw')) {
    const src = readFileSync(fileURLToPath(url.slice(0, -4)), 'utf8')
    return { format: 'module', source: `export default ${JSON.stringify(src)}`, shortCircuit: true }
  }
  return next(url, ctx)
}
