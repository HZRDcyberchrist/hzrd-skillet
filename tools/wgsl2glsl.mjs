// wgsl2glsl — a small WGSL -> GLSL 4.50 translator for the videoskillet
// shaders (MIT, Colin Diesh). It covers the subset of WGSL those shaders use:
// compute entry points with workgroup memory, storage buffers, a uniform
// block, sampled and storage 2D textures, and one vertex/fragment pair.
//
// WGSL infers the type of every `let`/`var`, and GLSL needs it spelled out,
// so this is a real front end: it parses, type-checks and concretizes the
// abstract numeric literals, then emits GLSL with WGSL's semantics preserved
// where the two languages differ (float %, select() argument order, vector
// comparisons, saturating float->int conversion, zero-initialized `var`).
//
// Output per entry point: GLSL source plus a binding table mapping each WGSL
// @binding to the GL resource kind and unit it was assigned.

// ───────────────────────────── lexer ─────────────────────────────

const PUNCT = [
  '<<=', '>>=', '->', '&&', '||', '==', '!=', '<=', '>=', '<<', '>>', '+=', '-=',
  '*=', '/=', '%=', '&=', '|=', '^=', '++', '--',
  '(', ')', '{', '}', '[', ']', '<', '>', ',', ';', ':', '.', '=', '+', '-', '*',
  '/', '%', '&', '|', '^', '!', '~', '@',
]

function lex(src) {
  const toks = []
  let i = 0
  let line = 1
  const n = src.length
  while (i < n) {
    const c = src[i]
    if (c === '\n') { line++; i++; continue }
    if (/\s/.test(c)) { i++; continue }
    if (c === '/' && src[i + 1] === '/') {
      while (i < n && src[i] !== '\n') i++
      continue
    }
    if (c === '/' && src[i + 1] === '*') {
      let depth = 1
      i += 2
      while (i < n && depth > 0) {
        if (src[i] === '\n') line++
        if (src[i] === '/' && src[i + 1] === '*') { depth++; i += 2 }
        else if (src[i] === '*' && src[i + 1] === '/') { depth--; i += 2 }
        else i++
      }
      continue
    }
    if (/[A-Za-z_]/.test(c)) {
      let j = i + 1
      while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++
      toks.push({ t: 'id', v: src.slice(i, j), line })
      i = j
      continue
    }
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1]))) {
      let j = i
      let text
      if (c === '0' && (src[i + 1] === 'x' || src[i + 1] === 'X')) {
        j = i + 2
        while (j < n && /[0-9a-fA-F]/.test(src[j])) j++
        text = src.slice(i, j)
        let suf = ''
        if (src[j] === 'u' || src[j] === 'i') { suf = src[j]; j++ }
        toks.push({ t: 'num', v: text, suf, isFloat: false, hex: true, line })
        i = j
        continue
      }
      while (j < n && /[0-9]/.test(src[j])) j++
      let isFloat = false
      if (src[j] === '.' && /[0-9eE]/.test(src[j + 1] ?? '') ) { isFloat = true; j++; while (j < n && /[0-9]/.test(src[j])) j++ }
      else if (src[j] === '.' && !/[A-Za-z_]/.test(src[j + 1] ?? '')) { isFloat = true; j++ }
      if (src[j] === 'e' || src[j] === 'E') {
        let k = j + 1
        if (src[k] === '+' || src[k] === '-') k++
        if (/[0-9]/.test(src[k])) {
          isFloat = true
          j = k
          while (j < n && /[0-9]/.test(src[j])) j++
        }
      }
      text = src.slice(i, j)
      let suf = ''
      if (src[j] === 'u' || src[j] === 'i') { suf = src[j]; j++ }
      else if (src[j] === 'f' || src[j] === 'h') { suf = 'f'; isFloat = true; j++ }
      toks.push({ t: 'num', v: text, suf, isFloat, hex: false, line })
      i = j
      continue
    }
    let matched = null
    for (const p of PUNCT) {
      if (src.startsWith(p, i)) { matched = p; break }
    }
    if (!matched) throw new Error(`lex: unexpected '${c}' at line ${line}`)
    toks.push({ t: 'p', v: matched, line })
    i += matched.length
  }
  toks.push({ t: 'eof', v: '', line })
  return toks
}

// ───────────────────────────── types ─────────────────────────────

const T = {
  f32: { k: 'scalar', s: 'f32' },
  i32: { k: 'scalar', s: 'i32' },
  u32: { k: 'scalar', s: 'u32' },
  bool: { k: 'scalar', s: 'bool' },
  absint: { k: 'scalar', s: 'absint' },
  absfloat: { k: 'scalar', s: 'absfloat' },
  void: { k: 'void' },
}
const vec = (n, s) => ({ k: 'vec', n, s })
const isAbs = s => s === 'absint' || s === 'absfloat'
const scalarOf = t => (t.k === 'scalar' || t.k === 'vec' ? t.s : t.k === 'mat' ? 'f32' : null)
const withScalar = (t, s) => (t.k === 'scalar' ? { k: 'scalar', s } : t.k === 'vec' ? vec(t.n, s) : t)
const tEq = (a, b) => JSON.stringify(a) === JSON.stringify(b)
const concreteS = s => (s === 'absint' ? 'i32' : s === 'absfloat' ? 'f32' : s)
const concrete = t => {
  if (t.k === 'scalar' || t.k === 'vec') return withScalar(t, concreteS(t.s))
  if (t.k === 'array') return { ...t, elem: concrete(t.elem) }
  return t
}
// Unify two scalar kinds the way WGSL's automatic conversion does.
function unifyS(a, b) {
  if (a === b) return a
  if (isAbs(a) && isAbs(b)) return 'absfloat'
  if (a === 'absint') return b
  if (a === 'absfloat') return b === 'f32' ? 'f32' : b === 'i32' || b === 'u32' ? null : b
  if (b === 'absint') return a
  if (b === 'absfloat') return a === 'f32' ? 'f32' : null
  return null
}

const SHORT_VEC = {
  vec2f: vec(2, 'f32'), vec3f: vec(3, 'f32'), vec4f: vec(4, 'f32'),
  vec2i: vec(2, 'i32'), vec3i: vec(3, 'i32'), vec4i: vec(4, 'i32'),
  vec2u: vec(2, 'u32'), vec3u: vec(3, 'u32'), vec4u: vec(4, 'u32'),
  vec2h: vec(2, 'f32'), vec3h: vec(3, 'f32'), vec4h: vec(4, 'f32'),
}
const SHORT_MAT = {}
for (const c of [2, 3, 4]) for (const r of [2, 3, 4]) SHORT_MAT[`mat${c}x${r}f`] = { k: 'mat', c, r }

// ───────────────────────────── parser ─────────────────────────────

class Parser {
  constructor(toks) { this.toks = toks; this.i = 0 }
  peek(o = 0) { return this.toks[this.i + o] }
  next() { return this.toks[this.i++] }
  is(v, o = 0) { const t = this.peek(o); return (t.t === 'p' || t.t === 'id') && t.v === v }
  eat(v) { if (this.is(v)) { this.i++; return true } return false }
  expect(v) {
    const t = this.next()
    if (t.v !== v) throw new Error(`parse: expected '${v}' got '${t.v}' at line ${t.line}`)
    return t
  }
  ident() {
    const t = this.next()
    if (t.t !== 'id') throw new Error(`parse: expected identifier got '${t.v}' at line ${t.line}`)
    return t.v
  }

  attrs() {
    const out = []
    while (this.is('@')) {
      this.next()
      const name = this.ident()
      const args = []
      if (this.eat('(')) {
        while (!this.is(')')) {
          args.push(this.expr())
          if (!this.eat(',')) break
        }
        this.expect(')')
      }
      out.push({ name, args })
    }
    return out
  }

  // Types are parsed to a syntax node and resolved later, because an array
  // size may name a constant that is only known once globals are collected.
  type() {
    const name = this.ident()
    const node = { name, args: [] }
    if (this.is('<')) {
      this.next()
      while (!this.is('>')) {
        // template args: a type, an access mode / address space / texel
        // format (bare identifiers), or a constant expression (array size)
        if (this.peek().t === 'id' && (this.is('<', 1) || this.is(',', 1) || this.is('>', 1))) {
          node.args.push({ ty: this.type() })
        } else {
          node.args.push({ ex: this.shiftExpr() })
        }
        if (!this.eat(',')) break
      }
      this.expect('>')
    }
    return node
  }

  module() {
    const decls = []
    while (this.peek().t !== 'eof') {
      if (this.eat(';')) continue
      const at = this.attrs()
      const t = this.peek()
      if (t.v === 'enable' || t.v === 'requires' || t.v === 'diagnostic') {
        while (!this.is(';')) this.next()
        this.next()
        continue
      }
      if (t.v === 'const_assert') { while (!this.is(';')) this.next(); this.next(); continue }
      if (t.v === 'struct') decls.push(this.struct(at))
      else if (t.v === 'fn') decls.push(this.fn(at))
      else if (t.v === 'const' || t.v === 'override') decls.push({ ...this.constDecl(), kind: 'const', attrs: at })
      else if (t.v === 'var') decls.push({ ...this.varDecl(), kind: 'gvar', attrs: at })
      else if (t.v === 'alias') {
        this.next()
        const name = this.ident()
        this.expect('=')
        const ty = this.type()
        this.expect(';')
        decls.push({ kind: 'alias', name, ty })
      } else throw new Error(`parse: unexpected '${t.v}' at line ${t.line}`)
    }
    return decls
  }

  struct(attrs) {
    this.expect('struct')
    const name = this.ident()
    this.expect('{')
    const fields = []
    while (!this.is('}')) {
      const fa = this.attrs()
      const fname = this.ident()
      this.expect(':')
      const ty = this.type()
      fields.push({ name: fname, ty, attrs: fa })
      if (!this.eat(',')) this.eat(';')
    }
    this.expect('}')
    this.eat(';')
    return { kind: 'struct', name, fields, attrs }
  }

  fn(attrs) {
    this.expect('fn')
    const name = this.ident()
    this.expect('(')
    const params = []
    while (!this.is(')')) {
      const pa = this.attrs()
      const pname = this.ident()
      this.expect(':')
      const ty = this.type()
      params.push({ name: pname, ty, attrs: pa })
      if (!this.eat(',')) break
    }
    this.expect(')')
    let ret = null
    let retAttrs = []
    if (this.eat('->')) {
      retAttrs = this.attrs()
      ret = this.type()
    }
    const body = this.block()
    return { kind: 'fn', name, params, ret, retAttrs, body, attrs }
  }

  constDecl() {
    this.next() // const / override
    const name = this.ident()
    let ty = null
    if (this.eat(':')) ty = this.type()
    let init = null
    if (this.eat('=')) init = this.expr()
    this.expect(';')
    return { name, ty, init }
  }

  varDecl() {
    this.expect('var')
    let space = null
    let access = null
    if (this.eat('<')) {
      space = this.ident()
      if (this.eat(',')) access = this.ident()
      this.expect('>')
    }
    const name = this.ident()
    let ty = null
    if (this.eat(':')) ty = this.type()
    let init = null
    if (this.eat('=')) init = this.expr()
    this.expect(';')
    return { name, space, access, ty, init }
  }

  block() {
    this.expect('{')
    const stmts = []
    while (!this.is('}')) stmts.push(this.stmt())
    this.expect('}')
    return { s: 'block', stmts }
  }

  stmt() {
    const t = this.peek()
    if (t.v === '{' && t.t === 'p') return this.block()
    if (t.v === ';' && t.t === 'p') { this.next(); return { s: 'empty' } }
    if (t.t === 'id') {
      switch (t.v) {
        case 'let':
        case 'const': {
          this.next()
          const name = this.ident()
          let ty = null
          if (this.eat(':')) ty = this.type()
          this.expect('=')
          const init = this.expr()
          this.expect(';')
          return { s: t.v, name, ty, init }
        }
        case 'var': {
          const d = this.varDecl()
          return { s: 'var', ...d }
        }
        case 'return': {
          this.next()
          let e = null
          if (!this.is(';')) e = this.expr()
          this.expect(';')
          return { s: 'return', e }
        }
        case 'if': return this.ifStmt()
        case 'for': return this.forStmt()
        case 'while': {
          this.next()
          const cond = this.expr()
          const body = this.block()
          return { s: 'while', cond, body }
        }
        case 'loop': {
          this.next()
          const body = this.block()
          return { s: 'loop', body }
        }
        case 'switch': return this.switchStmt()
        case 'break': {
          this.next()
          if (this.eat('if')) { const cond = this.expr(); this.expect(';'); return { s: 'breakif', cond } }
          this.expect(';')
          return { s: 'break' }
        }
        case 'continue': this.next(); this.expect(';'); return { s: 'continue' }
        case 'discard': this.next(); this.expect(';'); return { s: 'discard' }
        case 'const_assert': while (!this.is(';')) this.next(); this.next(); return { s: 'empty' }
      }
    }
    const st = this.simpleStmt()
    this.expect(';')
    return st
  }

  // assignment, increment, compound assignment or call
  simpleStmt() {
    if (this.is('_') && this.is('=', 1)) {
      this.next(); this.next()
      return { s: 'expr', e: this.expr() }
    }
    const lhs = this.unary()
    const t = this.peek()
    if (t.t === 'p') {
      if (t.v === '++' || t.v === '--') { this.next(); return { s: 'incdec', lhs, op: t.v } }
      if (t.v === '=') { this.next(); return { s: 'assign', lhs, op: '=', rhs: this.expr() } }
      if (['+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='].includes(t.v)) {
        this.next()
        return { s: 'assign', lhs, op: t.v, rhs: this.expr() }
      }
    }
    return { s: 'expr', e: lhs }
  }

  ifStmt() {
    this.expect('if')
    const cond = this.expr()
    const then = this.block()
    let els = null
    if (this.eat('else')) {
      if (this.is('if')) els = this.ifStmt()
      else els = this.block()
    }
    return { s: 'if', cond, then, els }
  }

  forStmt() {
    this.expect('for')
    this.expect('(')
    let init = null
    if (!this.is(';')) {
      if (this.is('var') || this.is('let') || this.is('const')) {
        init = this.stmt() // consumes the ';'
      } else {
        init = this.simpleStmt()
        this.expect(';')
      }
    } else this.expect(';')
    let cond = null
    if (!this.is(';')) cond = this.expr()
    this.expect(';')
    let update = null
    if (!this.is(')')) update = this.simpleStmt()
    this.expect(')')
    const body = this.block()
    return { s: 'for', init, cond, update, body }
  }

  switchStmt() {
    this.expect('switch')
    const e = this.expr()
    this.expect('{')
    const cases = []
    while (!this.is('}')) {
      const sels = []
      let isDefault = false
      if (this.eat('default')) {
        isDefault = true
      } else {
        this.expect('case')
        while (true) {
          if (this.eat('default')) isDefault = true
          else sels.push(this.expr())
          if (!this.eat(',')) break
          if (this.is(':') || this.is('{')) break
        }
      }
      this.eat(':')
      const body = this.block()
      cases.push({ sels, isDefault, body })
    }
    this.expect('}')
    return { s: 'switch', e, cases }
  }

  // ── expressions ── (WGSL has no precedence between mixed bitwise and
  // relational operators without parentheses; a conventional C ladder parses
  // every valid WGSL expression the same way)
  expr() { return this.logicalOr() }
  logicalOr() {
    let l = this.logicalAnd()
    while (this.is('||')) { this.next(); l = { e: 'bin', op: '||', l, r: this.logicalAnd() } }
    return l
  }
  logicalAnd() {
    let l = this.bitOr()
    while (this.is('&&')) { this.next(); l = { e: 'bin', op: '&&', l, r: this.bitOr() } }
    return l
  }
  bitOr() {
    let l = this.bitXor()
    while (this.is('|')) { this.next(); l = { e: 'bin', op: '|', l, r: this.bitXor() } }
    return l
  }
  bitXor() {
    let l = this.bitAnd()
    while (this.is('^')) { this.next(); l = { e: 'bin', op: '^', l, r: this.bitAnd() } }
    return l
  }
  bitAnd() {
    let l = this.equality()
    while (this.is('&')) { this.next(); l = { e: 'bin', op: '&', l, r: this.equality() } }
    return l
  }
  equality() {
    let l = this.relational()
    while (this.is('==') || this.is('!=')) { const op = this.next().v; l = { e: 'bin', op, l, r: this.relational() } }
    return l
  }
  relational() {
    let l = this.shiftExpr()
    while (this.is('<') || this.is('>') || this.is('<=') || this.is('>=')) {
      const op = this.next().v
      l = { e: 'bin', op, l, r: this.shiftExpr() }
    }
    return l
  }
  shiftExpr() {
    let l = this.additive()
    while (this.is('<<') || this.is('>>')) { const op = this.next().v; l = { e: 'bin', op, l, r: this.additive() } }
    return l
  }
  additive() {
    let l = this.multiplicative()
    while (this.is('+') || this.is('-')) { const op = this.next().v; l = { e: 'bin', op, l, r: this.multiplicative() } }
    return l
  }
  multiplicative() {
    let l = this.unary()
    while (this.is('*') || this.is('/') || this.is('%')) { const op = this.next().v; l = { e: 'bin', op, l, r: this.unary() } }
    return l
  }
  unary() {
    const t = this.peek()
    if (t.t === 'p' && ['-', '!', '~', '&', '*'].includes(t.v)) {
      this.next()
      return { e: 'un', op: t.v, x: this.unary() }
    }
    return this.postfix(this.primary())
  }
  postfix(x) {
    while (true) {
      if (this.is('.')) {
        this.next()
        x = { e: 'member', x, name: this.ident() }
      } else if (this.is('[')) {
        this.next()
        const idx = this.expr()
        this.expect(']')
        x = { e: 'index', x, idx }
      } else return x
    }
  }
  args() {
    this.expect('(')
    const a = []
    while (!this.is(')')) {
      a.push(this.expr())
      if (!this.eat(',')) break
    }
    this.expect(')')
    return a
  }
  primary() {
    const t = this.next()
    if (t.t === 'num') return { e: 'lit', tok: t }
    if (t.t === 'p' && t.v === '(') {
      const x = this.expr()
      this.expect(')')
      return { e: 'paren', x }
    }
    if (t.t === 'id') {
      if (t.v === 'true' || t.v === 'false') return { e: 'bool', v: t.v === 'true' }
      // templated constructor / bitcast: vec3<f32>(...), array<f32, 4>(...)
      if (this.is('<') && TEMPLATED.has(t.v)) {
        this.i--
        const ty = this.type()
        const args = this.args()
        return { e: 'call', name: ty.name, tmpl: ty, args }
      }
      if (this.is('(')) {
        return { e: 'call', name: t.v, args: this.args() }
      }
      return { e: 'id', name: t.v }
    }
    throw new Error(`parse: unexpected '${t.v}' at line ${t.line}`)
  }
}
const TEMPLATED = new Set(['vec2', 'vec3', 'vec4', 'mat2x2', 'mat2x3', 'mat2x4', 'mat3x2', 'mat3x3', 'mat3x4', 'mat4x2', 'mat4x3', 'mat4x4', 'array', 'bitcast', 'ptr', 'atomic'])

// ─────────────────────── GLSL name hygiene ───────────────────────

const GLSL_RESERVED = new Set(`attribute const uniform varying buffer shared coherent volatile restrict readonly writeonly atomic_uint layout centroid flat smooth noperspective patch sample break continue do for while switch case default if else subroutine in out inout float double int void bool true false invariant precise discard return mat2 mat3 mat4 dmat2 dmat3 dmat4 mat2x2 mat2x3 mat2x4 mat3x2 mat3x3 mat3x4 mat4x2 mat4x3 mat4x4 vec2 vec3 vec4 ivec2 ivec3 ivec4 bvec2 bvec3 bvec4 dvec2 dvec3 dvec4 uint uvec2 uvec3 uvec4 lowp mediump highp precision sampler1D sampler2D sampler3D samplerCube image1D image2D image3D struct common partition active asm class union enum typedef template this resource goto inline noinline public static extern external interface long short half fixed unsigned superp input output hvec2 hvec3 hvec4 fvec2 fvec3 fvec4 filter sizeof cast namespace using main texture`.split(/\s+/))
const GLSL_BUILTIN_FNS = new Set(`radians degrees sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh pow exp log exp2 log2 sqrt inversesqrt abs sign floor trunc round roundEven ceil fract mod modf min max clamp mix step smoothstep isnan isinf floatBitsToInt floatBitsToUint intBitsToFloat uintBitsToFloat fma frexp ldexp packUnorm2x16 packSnorm2x16 packUnorm4x8 packSnorm4x8 unpackUnorm2x16 unpackSnorm2x16 unpackUnorm4x8 unpackSnorm4x8 packHalf2x16 unpackHalf2x16 length distance dot cross normalize faceforward reflect refract matrixCompMult outerProduct transpose determinant inverse lessThan lessThanEqual greaterThan greaterThanEqual equal notEqual any all not uaddCarry usubBorrow umulExtended imulExtended bitfieldExtract bitfieldInsert bitfieldReverse bitCount findLSB findMSB textureSize texture textureLod texelFetch textureGather imageStore imageLoad imageSize barrier memoryBarrier memoryBarrierShared memoryBarrierBuffer memoryBarrierImage groupMemoryBarrier noise1 noise2 noise3 noise4 dFdx dFdy fwidth atomicAdd atomicMin atomicMax atomicAnd atomicOr atomicXor atomicExchange atomicCompSwap`.split(/\s+/))

// ───────────────────────── builtins table ─────────────────────────
// Each entry types a call and names its GLSL spelling. `gen` builtins take and
// return one type T (vector or scalar); arguments are unified to it.

const GEN_SAME = { // f(T,...)->T, glsl name
  abs: 'abs', sign: 'sign', floor: 'floor', ceil: 'ceil', fract: 'fract', trunc: 'trunc', round: 'roundEven',
  sqrt: 'sqrt', inverseSqrt: 'inversesqrt', exp: 'exp', exp2: 'exp2', log: 'log', log2: 'log2', pow: 'pow',
  sin: 'sin', cos: 'cos', tan: 'tan', asin: 'asin', acos: 'acos', atan: 'atan', sinh: 'sinh', cosh: 'cosh', tanh: 'tanh',
  asinh: 'asinh', acosh: 'acosh', atanh: 'atanh', atan2: 'atan', min: 'min', max: 'max', clamp: 'clamp', fma: 'fma',
  step: 'step', smoothstep: 'smoothstep', degrees: 'degrees', radians: 'radians', saturate: 'saturate',
  dpdx: 'dFdx', dpdy: 'dFdy', fwidth: 'fwidth', reverseBits: 'bitfieldReverse', normalize: 'normalize',
  reflect: 'reflect',
}

// ─────────────────────────── translator ───────────────────────────

export function translate(src, opts = {}) {
  const toks = lex(src)
  const decls = new Parser(toks).module()
  const tr = new Translator(decls, opts)
  return tr.run()
}

class Translator {
  constructor(decls, opts) {
    this.decls = decls
    this.opts = opts
    this.structs = new Map()
    this.consts = new Map() // name -> { type, value (number|null), decl }
    this.gvars = new Map()
    this.fns = new Map()
    this.aliases = new Map()
    this.renames = new Map() // wgsl global name -> glsl name
  }

  // global names that must not be used by GLSL: keywords, builtin functions,
  // anything reserved by the gl_ prefix or containing a double underscore.
  safe(name) {
    if (GLSL_RESERVED.has(name) || GLSL_BUILTIN_FNS.has(name) || name.startsWith('gl_') || name.includes('__')) {
      return `${name.replace(/__+/g, '_')}_w`
    }
    return name
  }

  run() {
    for (const d of this.decls) {
      if (d.kind === 'alias') this.aliases.set(d.name, d.ty)
    }
    for (const d of this.decls) {
      if (d.kind === 'struct') this.structs.set(d.name, d)
    }
    // constants first (types may depend on them), in order
    for (const d of this.decls) {
      if (d.kind === 'const') this.declareConst(d, this.consts, null)
    }
    for (const d of this.decls) {
      if (d.kind === 'struct') {
        d.resolved = d.fields.map(f => ({ name: f.name, type: this.resolveType(f.ty), attrs: f.attrs }))
      }
    }
    for (const d of this.decls) {
      if (d.kind === 'gvar') {
        this.gvars.set(d.name, { decl: d, type: d.ty ? this.resolveType(d.ty) : null })
      }
      if (d.kind === 'fn') this.fns.set(d.name, d)
    }
    for (const [name, g] of this.gvars) {
      if (!g.type && g.decl.init) g.type = concrete(this.typeOf(g.decl.init, this.globalScope()))
    }
    for (const d of this.decls) {
      if (d.kind === 'fn') {
        d.pTypes = d.params.map(p => this.resolveType(p.ty))
        d.rType = d.ret ? this.resolveType(d.ret) : T.void
      }
    }
    const entries = this.decls.filter(d => d.kind === 'fn' && d.attrs.some(a => ['compute', 'vertex', 'fragment'].includes(a.name)))
    const out = {}
    for (const e of entries) out[e.name] = this.emitEntry(e)
    return out
  }

  globalScope() {
    return { parent: null, vars: new Map() }
  }

  declareConst(d, table, scope) {
    let type = d.ty ? this.resolveType(d.ty) : null
    const sc = scope ?? this.globalScope()
    const it = this.typeOf(d.init, sc)
    if (!type) type = it
    let value = null
    try { value = this.evalConst(d.init) } catch { value = null }
    table.set(d.name, { type, value, decl: d })
  }

  resolveType(node) {
    if (!node) return null
    const { name, args } = node
    if (this.aliases.has(name)) return this.resolveType(this.aliases.get(name))
    if (T[name] && name !== 'void' && !isAbs(name)) return T[name]
    if (name === 'f16') return T.f32
    if (SHORT_VEC[name]) return SHORT_VEC[name]
    if (SHORT_MAT[name]) return SHORT_MAT[name]
    if (/^vec[234]$/.test(name)) return vec(+name[3], this.resolveType(args[0].ty).s)
    if (/^mat[234]x[234]$/.test(name)) return { k: 'mat', c: +name[3], r: +name[5] }
    if (name === 'array') {
      const elem = this.resolveType(args[0].ty)
      let n = null
      if (args[1]) {
        const a = args[1].ex ?? { e: 'id', name: args[1].ty.name }
        n = this.evalConst(a)
      }
      return { k: 'array', elem, n }
    }
    if (name === 'atomic') return { k: 'atomic', s: this.resolveType(args[0].ty).s }
    if (name === 'ptr') return { k: 'ptr', space: args[0].ty.name, inner: this.resolveType(args[1].ty) }
    if (name === 'texture_2d') return { k: 'tex2d', s: this.resolveType(args[0].ty).s }
    if (name === 'texture_storage_2d') return { k: 'stex2d', fmt: args[0].ty.name, access: args[1]?.ty.name ?? 'write' }
    if (name === 'texture_external') return { k: 'texext' }
    if (name === 'sampler') return { k: 'sampler' }
    if (this.structs.has(name)) return { k: 'struct', name }
    throw new Error(`unknown type ${name}`)
  }

  // ── constant evaluation (numbers only) ──
  evalConst(e) {
    switch (e.e) {
      case 'lit': return litValue(e.tok)
      case 'bool': return e.v ? 1 : 0
      case 'paren': return this.evalConst(e.x)
      case 'id': {
        const c = this.consts.get(e.name)
        if (!c || c.value === null) throw new Error(`not const: ${e.name}`)
        return c.value
      }
      case 'un': {
        const v = this.evalConst(e.x)
        if (e.op === '-') return -v
        if (e.op === '~') return ~v >>> 0
        if (e.op === '!') return v ? 0 : 1
        throw new Error('const un')
      }
      case 'bin': {
        const a = this.evalConst(e.l)
        const b = this.evalConst(e.r)
        const ta = this.typeOf(e.l, this.globalScope())
        const isInt = ta.k === 'scalar' && (ta.s === 'u32' || ta.s === 'i32' || ta.s === 'absint')
        const isU = ta.s === 'u32'
        switch (e.op) {
          case '+': return isU ? (a + b) >>> 0 : a + b
          case '-': return isU ? (a - b) >>> 0 : a - b
          case '*': return isU ? Math.imul(a, b) >>> 0 : isInt ? Math.imul(a, b) : a * b
          case '/': return isInt ? Math.trunc(a / b) : a / b
          case '%': return isInt ? a % b : a - b * Math.trunc(a / b)
          case '<<': return isU ? (a << b) >>> 0 : a << b
          case '>>': return isU ? a >>> b : a >> b
          case '&': return isU ? (a & b) >>> 0 : a & b
          case '|': return isU ? (a | b) >>> 0 : a | b
          case '^': return isU ? (a ^ b) >>> 0 : a ^ b
          case '<': return +(a < b)
          case '>': return +(a > b)
          case '<=': return +(a <= b)
          case '>=': return +(a >= b)
          case '==': return +(a === b)
          case '!=': return +(a !== b)
          case '&&': return +(a && b)
          case '||': return +(a || b)
        }
        throw new Error('const bin')
      }
      case 'call': {
        if (['u32', 'i32', 'f32'].includes(e.name) && e.args.length === 1) {
          const v = this.evalConst(e.args[0])
          if (e.name === 'f32') return v
          if (e.name === 'u32') return Math.max(0, Math.trunc(v)) >>> 0
          return Math.trunc(v) | 0
        }
        if (['min', 'max'].includes(e.name)) return Math[e.name](...e.args.map(a => this.evalConst(a)))
        if (e.name === 'abs') return Math.abs(this.evalConst(e.args[0]))
        if (['sin', 'cos', 'sqrt', 'exp', 'log', 'floor', 'ceil'].includes(e.name)) return Math[e.name](this.evalConst(e.args[0]))
        if (e.name === 'pow') return Math.pow(this.evalConst(e.args[0]), this.evalConst(e.args[1]))
        throw new Error(`const call ${e.name}`)
      }
    }
    throw new Error(`const expr ${e.e}`)
  }

  // ── scoping ──
  lookup(name, scope) {
    for (let s = scope; s; s = s.parent) {
      if (s.vars.has(name)) return s.vars.get(name)
    }
    if (this.consts.has(name)) return { kind: 'const', type: this.consts.get(name).type, glsl: this.safe(name), cinfo: this.consts.get(name) }
    if (this.gvars.has(name)) {
      const g = this.gvars.get(name)
      return { kind: 'gvar', type: g.type, glsl: g.glsl ?? this.safe(name), g }
    }
    return null
  }

  // ── typing ──
  typeOf(e, scope) {
    if (e.ty) return e.ty
    const t = this.typeOfRaw(e, scope)
    e.ty = t
    return t
  }

  typeOfRaw(e, scope) {
    switch (e.e) {
      case 'lit': {
        const tk = e.tok
        if (tk.suf === 'u') return T.u32
        if (tk.suf === 'i') return T.i32
        if (tk.suf === 'f') return T.f32
        return tk.isFloat ? T.absfloat : T.absint
      }
      case 'bool': return T.bool
      case 'paren': return this.typeOf(e.x, scope)
      case 'id': {
        const v = this.lookup(e.name, scope)
        if (!v) throw new Error(`unknown identifier ${e.name}`)
        return v.type
      }
      case 'un': {
        const t = this.typeOf(e.x, scope)
        if (e.op === '&') return { k: 'ptr', inner: t }
        if (e.op === '*') return t.k === 'ptr' ? t.inner : t
        if (e.op === '!') return t
        return t
      }
      case 'member': {
        let t = this.typeOf(e.x, scope)
        if (t.k === 'ptr') t = t.inner
        if (t.k === 'struct') {
          const s = this.structs.get(t.name)
          const f = s.resolved.find(f => f.name === e.name)
          if (!f) throw new Error(`no field ${e.name} in ${t.name}`)
          return f.type
        }
        if (t.k === 'vec') {
          const n = e.name.length
          return n === 1 ? { k: 'scalar', s: t.s } : vec(n, t.s)
        }
        throw new Error(`member on ${JSON.stringify(t)}`)
      }
      case 'index': {
        let t = this.typeOf(e.x, scope)
        this.typeOf(e.idx, scope)
        if (t.k === 'ptr') t = t.inner
        if (t.k === 'array') return t.elem
        if (t.k === 'vec') return { k: 'scalar', s: t.s }
        if (t.k === 'mat') return vec(t.r, 'f32')
        throw new Error(`index on ${JSON.stringify(t)}`)
      }
      case 'bin': return this.typeBin(e, scope)
      case 'call': return this.typeCall(e, scope)
    }
    throw new Error(`typeOf ${e.e}`)
  }

  typeBin(e, scope) {
    const a = this.typeOf(e.l, scope)
    const b = this.typeOf(e.r, scope)
    const op = e.op
    if (op === '&&' || op === '||') return T.bool
    if (op === '<<' || op === '>>') return a
    const cmp = ['<', '>', '<=', '>=', '==', '!='].includes(op)
    // matrix products
    if (op === '*' && (a.k === 'mat' || b.k === 'mat')) {
      if (a.k === 'mat' && b.k === 'vec') return vec(a.r, 'f32')
      if (a.k === 'vec' && b.k === 'mat') return vec(b.c, 'f32')
      if (a.k === 'mat' && b.k === 'mat') return { k: 'mat', c: b.c, r: a.r }
      return a.k === 'mat' ? a : b
    }
    const sa = scalarOf(a)
    const sb = scalarOf(b)
    const s = unifyS(sa, sb)
    if (s === null) throw new Error(`type mismatch ${sa} ${op} ${sb}`)
    let res
    if (a.k === 'vec') res = vec(a.n, s)
    else if (b.k === 'vec') res = vec(b.n, s)
    else res = { k: 'scalar', s }
    if (cmp) return res.k === 'vec' ? vec(res.n, 'bool') : T.bool
    return res
  }

  typeCall(e, scope) {
    const name = e.name
    const argT = () => e.args.map(a => this.typeOf(a, scope))
    // constructors / conversions
    if (e.tmpl) {
      if (name === 'bitcast') { argT(); return this.resolveType(e.tmpl.args[0].ty) }
      if (name === 'array') {
        const ts = argT()
        const t = this.resolveType(e.tmpl.args.length ? { name: 'array', args: e.tmpl.args } : null)
        return t ?? { k: 'array', elem: concrete(ts[0]), n: e.args.length }
      }
      argT()
      return this.resolveType(e.tmpl)
    }
    if (name === 'array') {
      const ts = argT()
      let s = ts[0]
      for (const t of ts) if (!isAbs(scalarOf(t) ?? '')) s = t
      return { k: 'array', elem: concrete(s), n: e.args.length }
    }
    if (T[name] && !isAbs(name) && name !== 'void') { argT(); return T[name] }
    if (SHORT_VEC[name]) { argT(); return SHORT_VEC[name] }
    if (SHORT_MAT[name]) { argT(); return SHORT_MAT[name] }
    if (/^vec[234]$/.test(name)) {
      // vec3(x) with inferred component type
      const ts = argT()
      let s = 'absint'
      for (const t of ts) s = unifyS(s, scalarOf(t)) ?? scalarOf(t)
      return vec(+name[3], s)
    }
    if (this.structs.has(name)) { argT(); return { k: 'struct', name } }
    if (this.fns.has(name)) {
      argT()
      return this.fns.get(name).rType
    }
    const ts = argT()
    if (GEN_SAME[name]) {
      if (name === 'atan2' || name === 'pow' || name === 'min' || name === 'max' || name === 'clamp' || name === 'fma' || name === 'step' || name === 'smoothstep' || name === 'reflect') {
        let r = ts[0]
        for (const t of ts) if (t.k === 'vec') r = t
        let s = scalarOf(ts[0])
        for (const t of ts) s = unifyS(s, scalarOf(t)) ?? s
        return withScalar(r, s)
      }
      return ts[0]
    }
    switch (name) {
      case 'mix': {
        let r = ts[0].k === 'vec' ? ts[0] : ts[1]
        return withScalar(r, unifyS(scalarOf(ts[0]), scalarOf(ts[1])) ?? scalarOf(r))
      }
      case 'select': {
        const s = unifyS(scalarOf(ts[0]), scalarOf(ts[1]))
        const base = ts[0].k === 'vec' ? ts[0] : ts[1]
        return withScalar(base, s ?? scalarOf(base))
      }
      case 'dot': return { k: 'scalar', s: concreteS(scalarOf(ts[0])) === 'absint' ? 'i32' : unifyS(scalarOf(ts[0]), scalarOf(ts[1])) }
      case 'length': case 'distance': case 'determinant': return T.f32
      case 'cross': return vec(3, 'f32')
      case 'transpose': return { k: 'mat', c: ts[0].r, r: ts[0].c }
      case 'all': case 'any': return T.bool
      case 'countOneBits': case 'firstLeadingBit': case 'firstTrailingBit': case 'extractBits': case 'insertBits': return ts[0]
      case 'arrayLength': return T.u32
      case 'pack2x16float': case 'pack4x8unorm': case 'pack4x8snorm': case 'pack2x16unorm': case 'pack2x16snorm': return T.u32
      case 'unpack2x16float': case 'unpack2x16unorm': case 'unpack2x16snorm': return vec(2, 'f32')
      case 'unpack4x8unorm': case 'unpack4x8snorm': return vec(4, 'f32')
      case 'quantizeToF16': return ts[0]
      case 'textureLoad': {
        const t = ts[0]
        if (t.k === 'stex2d') return vec(4, 'f32')
        return vec(4, t.s ?? 'f32')
      }
      case 'textureSample': case 'textureSampleLevel': case 'textureSampleBaseClampToEdge': return vec(4, 'f32')
      case 'textureDimensions': return vec(2, 'u32')
      case 'textureStore': case 'workgroupBarrier': case 'storageBarrier': case 'textureBarrier': return T.void
      case 'atomicAdd': case 'atomicSub': case 'atomicMax': case 'atomicMin': case 'atomicLoad': case 'atomicExchange': case 'atomicOr': case 'atomicAnd':
        return { k: 'scalar', s: ts[0].inner?.s ?? 'u32' }
      case 'atomicStore': return T.void
    }
    throw new Error(`unknown function ${name}`)
  }

  // ── emission ──

  glslType(t) {
    t = concrete(t)
    switch (t.k) {
      case 'scalar': return { f32: 'float', i32: 'int', u32: 'uint', bool: 'bool' }[t.s]
      case 'vec': return { f32: 'vec', i32: 'ivec', u32: 'uvec', bool: 'bvec' }[t.s] + t.n
      case 'mat': return t.c === t.r ? `mat${t.c}` : `mat${t.c}x${t.r}`
      case 'struct': return this.safe(t.name)
      case 'atomic': return t.s === 'i32' ? 'int' : 'uint'
      case 'array': return this.glslType(t.elem) // dims handled by decl
      case 'void': return 'void'
    }
    throw new Error(`glslType ${JSON.stringify(t)}`)
  }

  arrayDims(t) {
    let d = ''
    while (t.k === 'array') { d += t.n === null ? '[]' : `[${t.n}]`; t = t.elem }
    return d
  }
  baseType(t) { while (t.k === 'array') t = t.elem; return t }

  decl(t, name) {
    return `${this.glslType(this.baseType(t))} ${name}${this.arrayDims(t)}`
  }

  zeroOf(t) {
    t = concrete(t)
    switch (t.k) {
      case 'scalar': return { f32: '0.0', i32: '0', u32: '0u', bool: 'false' }[t.s]
      case 'vec': return `${this.glslType(t)}(${this.zeroOf({ k: 'scalar', s: t.s })})`
      case 'mat': return `${this.glslType(t)}(0.0)`
      case 'struct': {
        const s = this.structs.get(t.name)
        return `${this.safe(t.name)}(${s.resolved.map(f => this.zeroOf(f.type)).join(', ')})`
      }
      case 'array': {
        const z = this.zeroOf(t.elem)
        return `${this.glslType(t.elem)}${this.arrayDims(t.elem)}[${t.n}](${Array(t.n).fill(z).join(', ')})`
      }
    }
    throw new Error(`zeroOf ${JSON.stringify(t)}`)
  }

  lit(value, s, tok) {
    s = concreteS(s)
    if (s === 'u32') {
      if (tok && tok.hex) return `${tok.v}u`
      return `${Math.trunc(value) >>> 0}u`
    }
    if (s === 'i32') {
      if (tok && tok.hex) return `int(${tok.v}u)`
      return `${Math.trunc(value) | 0}`
    }
    if (s === 'bool') return value ? 'true' : 'false'
    // float
    if (tok && tok.isFloat && tok.suf !== 'f') {
      let t = tok.v
      if (t.endsWith('.')) t += '0'
      if (t.startsWith('.')) t = '0' + t
      return t
    }
    if (!Number.isFinite(value)) throw new Error('non-finite literal')
    let str = String(value)
    if (!/[.eE]/.test(str)) str += '.0'
    else if (/e/.test(str) && !/\./.test(str)) str = str.replace(/e/, '.0e')
    return str
  }

  // Emit expression e with its abstract numerics concretized toward scalar
  // type `want` (null = default concretization).
  ex(e, scope, want = null) {
    const t = this.typeOf(e, scope)
    switch (e.e) {
      case 'lit': {
        const s = isAbs(t.s) ? (want && !isAbs(want) ? want : concreteS(t.s)) : t.s
        return this.lit(litValue(e.tok), s, e.tok)
      }
      case 'bool': return e.v ? 'true' : 'false'
      case 'paren': return `(${this.ex(e.x, scope, want)})`
      case 'id': {
        const v = this.lookup(e.name, scope)
        if (v.kind === 'const' && v.cinfo && isAbs(scalarOf(v.type) ?? '') && v.cinfo.value !== null && v.type.k === 'scalar') {
          const s = want && !isAbs(want) ? want : concreteS(v.type.s)
          return this.lit(v.cinfo.value, s, null)
        }
        if (v.kind === 'local' && v.absConst && v.type.k === 'scalar') {
          const s = want && !isAbs(want) ? want : concreteS(v.type.s)
          return this.lit(v.absConst.value, s, null)
        }
        return v.glsl
      }
      case 'un': {
        if (e.op === '&') return this.ex(e.x, scope, want)
        if (e.op === '*') return this.ex(e.x, scope, want)
        if (e.op === '!' && t.k === 'vec') return `not(${this.ex(e.x, scope)})`
        if (e.op === '-' && t.k === 'scalar' && isAbs(t.s)) {
          // fold negative abstract literal so `-1` in u32 context is not produced
          const s = want && !isAbs(want) ? want : concreteS(t.s)
          if (e.x.e === 'lit') return this.lit(-litValue(e.x.tok), s, null)
        }
        return `${e.op}(${this.ex(e.x, scope, want)})`
      }
      case 'member': {
        let xt = this.typeOf(e.x, scope)
        if (xt.k === 'ptr') xt = xt.inner
        const base = this.ex(e.x, scope)
        if (xt.k === 'struct') return `${base}.${this.safe(e.name)}`
        // swizzles: rgba -> xyzw
        const sw = e.name.replace(/r/g, 'x').replace(/g/g, 'y').replace(/b/g, 'z').replace(/a/g, 'w')
        return `${base}.${sw}`
      }
      case 'index': {
        const idx = this.ex(e.idx, scope, 'i32')
        return `${this.ex(e.x, scope)}[${idx}]`
      }
      case 'bin': return this.exBin(e, scope, want, t)
      case 'call': return this.exCall(e, scope, want, t)
    }
    throw new Error(`ex ${e.e}`)
  }

  exBin(e, scope, want, t) {
    const a = this.typeOf(e.l, scope)
    const b = this.typeOf(e.r, scope)
    const op = e.op
    if (op === '&&' || op === '||') return `(${this.ex(e.l, scope)} ${op} ${this.ex(e.r, scope)})`
    if (op === '<<' || op === '>>') {
      return `(${this.ex(e.l, scope, want)} ${op} ${this.ex(e.r, scope, 'u32')})`
    }
    // the scalar type both sides are converted to
    let s = unifyS(scalarOf(a), scalarOf(b))
    if (a.k === 'mat' || b.k === 'mat') s = 'f32'
    if (isAbs(s)) {
      const cmp = ['<', '>', '<=', '>=', '==', '!='].includes(op)
      s = !cmp && want && !isAbs(want) && want !== 'bool' ? want : concreteS(s)
    }
    const L = this.ex(e.l, scope, s)
    const R = this.ex(e.r, scope, s)
    const vecish = a.k === 'vec' || b.k === 'vec'
    if (['<', '>', '<=', '>=', '==', '!='].includes(op) && vecish) {
      const fn = { '<': 'lessThan', '>': 'greaterThan', '<=': 'lessThanEqual', '>=': 'greaterThanEqual', '==': 'equal', '!=': 'notEqual' }[op]
      const n = a.k === 'vec' ? a.n : b.n
      const vt = this.glslType(vec(n, s))
      const LL = a.k === 'vec' ? L : `${vt}(${L})`
      const RR = b.k === 'vec' ? R : `${vt}(${R})`
      return `${fn}(${LL}, ${RR})`
    }
    if (s === 'bool' && ['&', '|', '^'].includes(op)) {
      if (vecish) {
        const n = a.k === 'vec' ? a.n : b.n
        return `bvec${n}(uvec${n}(${L}) ${op} uvec${n}(${R}))`
      }
      return `(${L} ${op === '&' ? '&&' : op === '|' ? '||' : '!='} ${R})`
    }
    if (op === '%') {
      if (s === 'f32') {
        this.needs.add('fmod')
        return `wg_fmod(${L}, ${R})`
      }
      if (s === 'i32') {
        this.needs.add('imod')
        return `wg_imod(${L}, ${R})`
      }
    }
    return `(${L} ${op} ${R})`
  }

  convert(arg, from, to, scope) {
    // WGSL value conversion to scalar/vector type `to`
    const fs = scalarOf(from)
    const src = this.ex(arg, scope, isAbs(fs) ? (to.s === 'bool' ? null : to.s) : null)
    const gt = this.glslType(to)
    if (fs === to.s || (isAbs(fs) && to.s !== 'bool')) {
      if (from.k === to.k && (from.k !== 'vec' || from.n === to.n)) return fs === to.s ? src : `${gt}(${src})`
    }
    const cf = concreteS(fs)
    if ((cf === 'f32') && (to.s === 'u32')) {
      this.needs.add('f2u')
      return `wg_f2u(${src})`
    }
    if ((cf === 'f32') && (to.s === 'i32')) {
      this.needs.add('f2i')
      return `wg_f2i(${src})`
    }
    return `${gt}(${src})`
  }

  exCall(e, scope, want, t) {
    const name = e.name
    const args = e.args
    const argTs = args.map(a => this.typeOf(a, scope))
    // bitcast
    if (e.tmpl && name === 'bitcast') {
      const to = this.resolveType(e.tmpl.args[0].ty)
      const from = argTs[0]
      const x = this.ex(args[0], scope)
      const fs = concreteS(scalarOf(from))
      if (fs === to.s) return x
      if (fs === 'f32' && to.s === 'u32') return `floatBitsToUint(${x})`
      if (fs === 'f32' && to.s === 'i32') return `floatBitsToInt(${x})`
      if (fs === 'u32' && to.s === 'f32') return `uintBitsToFloat(${x})`
      if (fs === 'i32' && to.s === 'f32') return `intBitsToFloat(${x})`
      return `${this.glslType(to)}(${x})`
    }
    // constructors
    const ctorT = e.tmpl && name !== 'bitcast' ? this.resolveType(e.tmpl.args.length ? e.tmpl : null) : null
    const isCtor = ctorT || (T[name] && !isAbs(name) && name !== 'void') || SHORT_VEC[name] || SHORT_MAT[name] || /^vec[234]$/.test(name) || name === 'array'
    if (isCtor) {
      const rt = concrete(t)
      if (rt.k === 'array') {
        const es = rt.elem
        const parts = args.map(a => this.ex(a, scope, scalarOf(es)))
        return `${this.glslType(es)}${this.arrayDims(es)}[${rt.n}](${parts.join(', ')})`
      }
      if (rt.k === 'scalar' && args.length === 1) return this.convert(args[0], argTs[0], rt, scope)
      if (rt.k === 'vec' && args.length === 1 && argTs[0].k === 'vec') return this.convert(args[0], argTs[0], rt, scope)
      if (rt.k === 'vec' && args.length === 0) return this.zeroOf(rt)
      if (rt.k === 'mat' && args.length === 0) return this.zeroOf(rt)
      const s = scalarOf(rt)
      const parts = args.map((a, i) => {
        const at = argTs[i]
        const as = scalarOf(at)
        if (!isAbs(as) && as !== s && s) {
          // component-wise conversion inside a constructor
          return this.convert(a, at, at.k === 'vec' ? vec(at.n, s) : { k: 'scalar', s }, scope)
        }
        return this.ex(a, scope, s)
      })
      return `${this.glslType(rt)}(${parts.join(', ')})`
    }
    if (this.structs.has(name)) {
      const st = this.structs.get(name)
      const parts = args.map((a, i) => this.ex(a, scope, scalarOf(st.resolved[i].type)))
      return `${this.safe(name)}(${parts.join(', ')})`
    }
    if (this.fns.has(name)) {
      const f = this.fns.get(name)
      const parts = args.map((a, i) => {
        const pt = f.pTypes[i]
        if (pt.k === 'tex2d' || pt.k === 'stex2d' || pt.k === 'sampler') return this.ex(a, scope)
        return this.ex(a, scope, scalarOf(pt))
      })
      // samplers are not GLSL values; drop them from user calls
      const kept = parts.filter((_, i) => f.pTypes[i].k !== 'sampler')
      return `${this.fnName(name)}(${kept.join(', ')})`
    }
    // unify argument scalar types for generic builtins
    const unified = () => {
      let s = null
      for (const at of argTs) {
        const as = scalarOf(at)
        if (as === null) continue
        s = s === null ? as : (unifyS(s, as) ?? s)
      }
      if (s && isAbs(s)) s = want && !isAbs(want) && want !== 'bool' ? want : concreteS(s)
      return s
    }
    if (GEN_SAME[name]) {
      const s = unified()
      const parts = args.map(a => this.ex(a, scope, s))
      const g = GEN_SAME[name]
      if (name === 'saturate') {
        const z = this.lit(0, s)
        const o = this.lit(1, s)
        return `clamp(${parts[0]}, ${z}, ${o})`
      }
      if (name === 'abs' && (s === 'u32')) return parts[0]
      // WGSL allows (vec, vec, vec) only; GLSL also accepts scalar edges for
      // clamp/min/max/step/smoothstep, so pass through as is.
      return `${g}(${parts.join(', ')})`
    }
    switch (name) {
      case 'mix': {
        const s = unifyS(scalarOf(argTs[0]), scalarOf(argTs[1])) ?? 'f32'
        const cs = isAbs(s) ? 'f32' : s
        return `mix(${this.ex(args[0], scope, cs)}, ${this.ex(args[1], scope, cs)}, ${this.ex(args[2], scope, cs)})`
      }
      case 'select': {
        const s0 = unifyS(scalarOf(argTs[0]), scalarOf(argTs[1]))
        const s = isAbs(s0) ? (want && !isAbs(want) && want !== 'bool' ? want : concreteS(s0)) : s0
        const f = this.ex(args[0], scope, s)
        const tr = this.ex(args[1], scope, s)
        const c = this.ex(args[2], scope)
        if (argTs[2].k === 'vec') {
          const n = argTs[2].n
          const vt = this.glslType(vec(n, s))
          const F = argTs[0].k === 'vec' ? f : `${vt}(${f})`
          const TT = argTs[1].k === 'vec' ? tr : `${vt}(${tr})`
          return `mix(${F}, ${TT}, ${c})`
        }
        return `((${c}) ? (${tr}) : (${f}))`
      }
      case 'dot': case 'cross': case 'length': case 'distance': case 'determinant': case 'transpose': case 'all': case 'any': {
        const s = unified()
        return `${name}(${args.map(a => this.ex(a, scope, s)).join(', ')})`
      }
      case 'countOneBits': {
        const x = this.ex(args[0], scope, 'u32')
        const at = concrete(argTs[0])
        return at.k === 'vec' ? `${this.glslType(at)}(bitCount(${x}))` : `${this.glslType(at)}(bitCount(${x}))`
      }
      case 'firstLeadingBit': {
        const at = concrete(argTs[0])
        return `${this.glslType(at)}(findMSB(${this.ex(args[0], scope)}))`
      }
      case 'firstTrailingBit': {
        const at = concrete(argTs[0])
        return `${this.glslType(at)}(findLSB(${this.ex(args[0], scope)}))`
      }
      case 'extractBits': return `bitfieldExtract(${this.ex(args[0], scope)}, int(${this.ex(args[1], scope, 'u32')}), int(${this.ex(args[2], scope, 'u32')}))`
      case 'insertBits': return `bitfieldInsert(${this.ex(args[0], scope)}, ${this.ex(args[1], scope)}, int(${this.ex(args[2], scope, 'u32')}), int(${this.ex(args[3], scope, 'u32')}))`
      case 'arrayLength': return `uint(${this.ex(args[0], scope)}.length())`
      case 'pack2x16float': return `packHalf2x16(${this.ex(args[0], scope, 'f32')})`
      case 'unpack2x16float': return `unpackHalf2x16(${this.ex(args[0], scope, 'u32')})`
      case 'pack4x8unorm': return `packUnorm4x8(${this.ex(args[0], scope, 'f32')})`
      case 'unpack4x8unorm': return `unpackUnorm4x8(${this.ex(args[0], scope, 'u32')})`
      case 'pack4x8snorm': return `packSnorm4x8(${this.ex(args[0], scope, 'f32')})`
      case 'unpack4x8snorm': return `unpackSnorm4x8(${this.ex(args[0], scope, 'u32')})`
      case 'pack2x16unorm': return `packUnorm2x16(${this.ex(args[0], scope, 'f32')})`
      case 'unpack2x16unorm': return `unpackUnorm2x16(${this.ex(args[0], scope, 'u32')})`
      case 'quantizeToF16': return `unpackHalf2x16(packHalf2x16(vec2(${this.ex(args[0], scope, 'f32')}, 0.0))).x`
      case 'workgroupBarrier': return 'memoryBarrierShared(); barrier()'
      case 'storageBarrier': return 'memoryBarrierBuffer(); barrier()'
      case 'textureBarrier': return 'memoryBarrierImage(); barrier()'
      case 'textureLoad': {
        const tt = argTs[0]
        const tx = this.ex(args[0], scope)
        const c = `ivec2(${this.ex(args[1], scope, 'i32')})`
        if (tt.k === 'stex2d') return `imageLoad(${tx}, ${c})`
        const lvl = args[2] ? `int(${this.ex(args[2], scope, 'i32')})` : '0'
        return `texelFetch(${tx}, ${c}, ${lvl})`
      }
      case 'textureSampleLevel': return `textureLod(${this.ex(args[0], scope)}, ${this.ex(args[2], scope, 'f32')}, float(${this.ex(args[3], scope, 'f32')}))`
      case 'textureSample': return `texture(${this.ex(args[0], scope)}, ${this.ex(args[2], scope, 'f32')})`
      case 'textureSampleBaseClampToEdge': return `textureLod(${this.ex(args[0], scope)}, ${this.ex(args[2], scope, 'f32')}, 0.0)`
      case 'textureDimensions': {
        const tt = argTs[0]
        const tx = this.ex(args[0], scope)
        if (tt.k === 'stex2d') return `uvec2(imageSize(${tx}))`
        const lvl = args[1] ? `int(${this.ex(args[1], scope, 'i32')})` : '0'
        return `uvec2(textureSize(${tx}, ${lvl}))`
      }
      case 'textureStore': {
        return `imageStore(${this.ex(args[0], scope)}, ivec2(${this.ex(args[1], scope, 'i32')}), ${this.ex(args[2], scope, 'f32')})`
      }
      case 'atomicAdd': return `atomicAdd(${this.ex(args[0], scope)}, ${this.ex(args[1], scope, scalarOf(t))})`
      case 'atomicSub': return `atomicAdd(${this.ex(args[0], scope)}, -(${this.ex(args[1], scope, scalarOf(t))}))`
      case 'atomicMax': return `atomicMax(${this.ex(args[0], scope)}, ${this.ex(args[1], scope, scalarOf(t))})`
      case 'atomicMin': return `atomicMin(${this.ex(args[0], scope)}, ${this.ex(args[1], scope, scalarOf(t))})`
      case 'atomicOr': return `atomicOr(${this.ex(args[0], scope)}, ${this.ex(args[1], scope, scalarOf(t))})`
      case 'atomicAnd': return `atomicAnd(${this.ex(args[0], scope)}, ${this.ex(args[1], scope, scalarOf(t))})`
      case 'atomicExchange': return `atomicExchange(${this.ex(args[0], scope)}, ${this.ex(args[1], scope, scalarOf(t))})`
      case 'atomicLoad': return `atomicAdd(${this.ex(args[0], scope)}, ${this.lit(0, scalarOf(t))})`
      case 'atomicStore': return `atomicExchange(${this.ex(args[0], scope)}, ${this.ex(args[1], scope, argTs[0].inner?.s ?? 'u32')})`
    }
    throw new Error(`emit call ${name}`)
  }

  fnName(name) {
    return this.safe(name)
  }

  // ── statements ──

  localName(name, scope) {
    // Hide GLSL keywords/builtins, and never let a local shadow a function the
    // body may still call (GLSL resolves the name to the variable).
    let g = this.safe(name)
    if (this.fns.has(name) || g !== name) g = `${name}_v`
    if (GLSL_RESERVED.has(g) || GLSL_BUILTIN_FNS.has(g)) g = `${g}_v`
    return g
  }

  stmts(block, scope, ind) {
    const inner = { parent: scope, vars: new Map() }
    return block.stmts.map(s => this.stmt(s, inner, ind)).join('')
  }

  stmt(s, scope, ind) {
    const I = '  '.repeat(ind)
    switch (s.s) {
      case 'empty': return ''
      case 'block': return `${I}{\n${this.stmts(s, scope, ind + 1)}${I}}\n`
      case 'let':
      case 'const':
      case 'var': {
        let t = s.ty ? this.resolveType(s.ty) : null
        const it = s.init ? this.typeOf(s.init, scope) : null
        if (!t) t = concrete(it)
        const g = this.localName(s.name, scope)
        const entry = { kind: 'local', type: t, glsl: g }
        if (s.s === 'const' && it && it.k === 'scalar' && isAbs(it.s) && !s.ty) {
          // abstract local const: inline like a global one
          try {
            entry.absConst = { value: this.evalConstLocal(s.init, scope) }
            entry.type = it
            scope.vars.set(s.name, entry)
            return ''
          } catch { /* fall through to a typed const */ }
        }
        let init
        if (s.init) init = this.ex(s.init, scope, scalarOf(t))
        else init = this.zeroOf(t)
        scope.vars.set(s.name, entry)
        const q = s.s === 'const' ? 'const ' : ''
        return `${I}${q}${this.decl(t, g)} = ${init};\n`
      }
      case 'assign': {
        const lt = this.typeOf(s.lhs, scope)
        const L = this.ex(s.lhs, scope)
        let st = scalarOf(lt.k === 'ptr' ? lt.inner : lt)
        if (s.op === '=') return `${I}${L} = ${this.ex(s.rhs, scope, st)};\n`
        const op = s.op.slice(0, -1)
        if (op === '<<' || op === '>>') return `${I}${L} ${s.op} ${this.ex(s.rhs, scope, 'u32')};\n`
        const rt = this.typeOf(s.rhs, scope)
        if (op === '%' && concreteS(st) === 'f32') { this.needs.add('fmod'); return `${I}${L} = wg_fmod(${L}, ${this.ex(s.rhs, scope, 'f32')});\n` }
        if (op === '%' && concreteS(st) === 'i32') { this.needs.add('imod'); return `${I}${L} = wg_imod(${L}, ${this.ex(s.rhs, scope, 'i32')});\n` }
        if (st === 'bool' && ['&', '|', '^'].includes(op)) {
          const R = this.ex(s.rhs, scope)
          if (lt.k === 'vec') return `${I}${L} = bvec${lt.n}(uvec${lt.n}(${L}) ${op} uvec${lt.n}(${R}));\n`
          return `${I}${L} = (${L} ${op === '&' ? '&&' : op === '|' ? '||' : '!='} ${R});\n`
        }
        void rt
        return `${I}${L} ${s.op} ${this.ex(s.rhs, scope, st)};\n`
      }
      case 'incdec': return `${I}${this.ex(s.lhs, scope)}${s.op};\n`
      case 'expr': {
        const x = this.ex(s.e, scope)
        return `${I}${x};\n`
      }
      case 'return': {
        if (this.cur.stage === 'fragment' && s.e) {
          return `${I}{ ${this.cur.fragOut} = ${this.ex(s.e, scope, 'f32')}; return; }\n`
        }
        if (this.cur.stage === 'vertex' && s.e) {
          const r = this.ex(s.e, scope)
          const outs = this.cur.vsOuts.map(o => `${o.target} = _r.${this.safe(o.field)};`).join(' ')
          return `${I}{ ${this.glslType(this.cur.fn.rType)} _r = ${r}; ${outs} return; }\n`
        }
        if (this.cur.isEntry) return `${I}return;\n`
        return s.e ? `${I}return ${this.ex(s.e, scope, scalarOf(this.cur.fn.rType))};\n` : `${I}return;\n`
      }
      case 'if': {
        let out = `${I}if (${this.ex(s.cond, scope)}) {\n${this.stmts(s.then, scope, ind + 1)}${I}}`
        let els = s.els
        while (els) {
          if (els.s === 'if') {
            out += ` else if (${this.ex(els.cond, scope)}) {\n${this.stmts(els.then, scope, ind + 1)}${I}}`
            els = els.els
          } else {
            out += ` else {\n${this.stmts(els, scope, ind + 1)}${I}}`
            els = null
          }
        }
        return out + '\n'
      }
      case 'for': {
        const fs = { parent: scope, vars: new Map() }
        let init = ''
        if (s.init) init = this.stmt(s.init, fs, 0).trim().replace(/;$/, '')
        const cond = s.cond ? this.ex(s.cond, fs) : ''
        const upd = s.update ? this.stmt(s.update, fs, 0).trim().replace(/;$/, '') : ''
        return `${I}for (${init}; ${cond}; ${upd}) {\n${this.stmts(s.body, fs, ind + 1)}${I}}\n`
      }
      case 'while': return `${I}while (${this.ex(s.cond, scope)}) {\n${this.stmts(s.body, scope, ind + 1)}${I}}\n`
      case 'loop': return `${I}while (true) {\n${this.stmts(s.body, scope, ind + 1)}${I}}\n`
      case 'breakif': return `${I}if (${this.ex(s.cond, scope)}) { break; }\n`
      case 'break': return `${I}break;\n`
      case 'continue': return `${I}continue;\n`
      case 'discard': return `${I}discard;\n`
      case 'switch': {
        const st = concrete(this.typeOf(s.e, scope))
        let out = `${I}switch (${this.ex(s.e, scope)}) {\n`
        for (const c of s.cases) {
          const labels = c.sels.map(x => `case ${this.ex(x, scope, st.s)}:`)
          if (c.isDefault) labels.push('default:')
          out += `${I}  ${labels.join(' ')} {\n${this.stmts(c.body, scope, ind + 2)}${I}  } break;\n`
        }
        return out + `${I}}\n`
      }
    }
    throw new Error(`stmt ${s.s}`)
  }

  evalConstLocal(e, scope) {
    // local abstract consts only ever reference literals and global consts
    void scope
    return this.evalConst(e)
  }

  // ── functions and entry points ──

  emitFn(f) {
    this.cur = { fn: f, isEntry: false, stage: null }
    const scope = { parent: null, vars: new Map() }
    const ps = []
    f.params.forEach((p, i) => {
      const t = f.pTypes[i]
      const g = this.localName(p.name, scope)
      scope.vars.set(p.name, { kind: 'local', type: t, glsl: g })
      if (t.k === 'sampler') return
      if (t.k === 'tex2d') { ps.push(`sampler2D ${g}`); return }
      if (t.k === 'ptr') { ps.push(`inout ${this.decl(t.inner, g)}`); return }
      ps.push(this.decl(t, g))
    })
    const rt = this.glslType(f.rType)
    const body = this.stmts(f.body, scope, 1)
    const sig = `${rt}${f.rType.k === 'array' ? this.arrayDims(f.rType) : ''} ${this.fnName(f.name)}(${ps.join(', ')})`
    // WGSL resolves functions in any order; GLSL needs a prototype first.
    this.protos.push(`${sig};`)
    return `${sig} {\n${body}}\n`
  }

  // Which declarations an entry point reaches, so each generated program
  // carries only its own resources (GL binding units are per program).
  reach(entry) {
    const fns = new Set()
    const gvars = new Set()
    const visitE = (e) => {
      if (!e || typeof e !== 'object') return
      if (Array.isArray(e)) { e.forEach(visitE); return }
      if (e.e === 'id' && this.gvars.has(e.name)) gvars.add(e.name)
      if (e.e === 'call' && this.fns.has(e.name) && !fns.has(e.name)) {
        fns.add(e.name)
        visitE(this.fns.get(e.name).body)
      }
      for (const k of Object.keys(e)) {
        if (k === 'ty' || k === 'tok') continue
        const v = e[k]
        if (v && typeof v === 'object') visitE(v)
      }
    }
    visitE(entry.body)
    return { fns, gvars }
  }

  emitEntry(entry) {
    const stage = entry.attrs.find(a => ['compute', 'vertex', 'fragment'].includes(a.name)).name
    this.needs = new Set()
    const { fns, gvars } = this.reach(entry)
    // function order: keep source order (WGSL allows any order; GLSL needs
    // declaration before use, and the prelude + shader are written in order)
    const fnDecls = this.decls.filter(d => d.kind === 'fn' && fns.has(d.name))
    const parts = []
    const bindings = []
    const counters = { ubo: 0, ssbo: 0, tex: 0, image: 0 }
    const resDecl = []
    for (const d of this.decls) {
      if (d.kind !== 'gvar' || !gvars.has(d.name)) continue
      const g = this.gvars.get(d.name)
      const t = g.type
      const name = this.safe(d.name)
      g.glsl = name
      const bAttr = d.attrs.find(a => a.name === 'binding')
      const binding = bAttr ? this.evalConst(bAttr.args[0]) : null
      if (d.space === 'uniform') {
        const unit = counters.ubo++
        bindings.push({ binding, kind: 'ubo', unit, name: d.name })
        resDecl.push(`layout(std140, binding = ${unit}) uniform UBO_${name} { ${this.decl(t, name)}; };`)
      } else if (d.space === 'storage') {
        const unit = counters.ssbo++
        bindings.push({ binding, kind: 'ssbo', unit, name: d.name, access: d.access ?? 'read' })
        const q = (d.access ?? 'read') === 'read' ? 'readonly ' : ''
        resDecl.push(`layout(std430, binding = ${unit}) ${q}buffer SSBO_${name} { ${this.decl(t, name)}; };`)
      } else if (d.space === 'workgroup') {
        resDecl.push(`shared ${this.decl(t, name)};`)
      } else if (d.space === 'private') {
        resDecl.push(`${this.decl(t, name)} = ${d.init ? this.ex(d.init, this.globalScope(), scalarOf(t)) : this.zeroOf(t)};`)
      } else if (t.k === 'tex2d') {
        const unit = counters.tex++
        bindings.push({ binding, kind: 'tex', unit, name: d.name })
        resDecl.push(`layout(binding = ${unit}) uniform sampler2D ${name};`)
      } else if (t.k === 'stex2d') {
        const unit = counters.image++
        const fmt = { rgba8unorm: 'rgba8', r32float: 'r32f', rgba16float: 'rgba16f', rgba32float: 'rgba32f', r32uint: 'r32ui' }[t.fmt]
        bindings.push({ binding, kind: 'image', unit, name: d.name, format: t.fmt })
        const q = t.access === 'read' ? 'readonly' : t.access === 'read_write' ? '' : 'writeonly'
        resDecl.push(`layout(binding = ${unit}, ${fmt}) ${q} uniform image2D ${name};`)
      } else if (t.k === 'sampler') {
        bindings.push({ binding, kind: 'sampler', unit: -1, name: d.name })
      } else if (t.k === 'texext') {
        throw new Error('texture_external is browser-only')
      }
    }
    // Samplers are merged into the texture units they are used with; GL
    // samples through the texture unit's own filter state.
    // structs
    const structOut = []
    for (const d of this.decls) {
      if (d.kind !== 'struct') continue
      if (entry.rType && entry.rType.k === 'struct' && entry.rType.name === d.name && stage === 'fragment') continue
      structOut.push(`struct ${this.safe(d.name)} {\n${d.resolved.map(f => `  ${this.decl(f.type, this.safe(f.name))};`).join('\n')}\n};`)
    }
    // consts (typed ones as GLSL consts; abstract scalars are inlined at use)
    const constOut = []
    for (const [name, c] of this.consts) {
      const ct = c.type
      if (ct.k === 'scalar' && isAbs(ct.s) && c.value !== null) continue
      const t = concrete(ct)
      let init
      if (c.value !== null && t.k === 'scalar') init = this.lit(c.value, t.s, null)
      else init = this.ex(c.decl.init, this.globalScope(), scalarOf(t))
      constOut.push(`const ${this.decl(t, this.safe(name))} = ${init};`)
    }
    // functions
    this.protos = []
    const fnOut = fnDecls.map(f => this.emitFn(f))
    // entry
    this.cur = { fn: entry, isEntry: true, stage }
    const scope = { parent: null, vars: new Map() }
    let pre = ''
    let head = ''
    const ioDecl = []
    if (stage === 'compute') {
      const ws = entry.attrs.find(a => a.name === 'workgroup_size').args.map(a => this.evalConst(a))
      head = `layout(local_size_x = ${ws[0]}, local_size_y = ${ws[1] ?? 1}, local_size_z = ${ws[2] ?? 1}) in;`
      this.workgroupSize = ws
    }
    const BUILTIN = {
      global_invocation_id: 'gl_GlobalInvocationID', local_invocation_id: 'gl_LocalInvocationID',
      workgroup_id: 'gl_WorkGroupID', num_workgroups: 'gl_NumWorkGroups', local_invocation_index: 'gl_LocalInvocationIndex',
      vertex_index: 'uint(gl_VertexID)', instance_index: 'uint(gl_InstanceID)', position: 'gl_FragCoord',
      front_facing: 'gl_FrontFacing',
    }
    entry.params.forEach((p, i) => {
      const t = entry.pTypes[i]
      const g = this.localName(p.name, scope)
      scope.vars.set(p.name, { kind: 'local', type: t, glsl: g })
      const b = p.attrs.find(a => a.name === 'builtin')
      const loc = p.attrs.find(a => a.name === 'location')
      if (b) {
        pre += `  ${this.decl(t, g)} = ${BUILTIN[b.args[0].name]};\n`
      } else if (loc) {
        const l = this.evalConst(loc.args[0])
        ioDecl.push(`layout(location = ${l}) in ${this.decl(t, `v_loc${l}`)};`)
        pre += `  ${this.decl(t, g)} = v_loc${l};\n`
      } else if (t.k === 'struct') {
        // fragment input struct
        const st = this.structs.get(t.name)
        const fields = st.resolved.map(f => {
          const fb = f.attrs.find(a => a.name === 'builtin')
          const fl = f.attrs.find(a => a.name === 'location')
          if (fb) return BUILTIN[fb.args[0].name]
          const l = this.evalConst(fl.args[0])
          ioDecl.push(`layout(location = ${l}) in ${this.decl(f.type, `v_loc${l}`)};`)
          return `v_loc${l}`
        })
        pre += `  ${this.safe(t.name)} ${g} = ${this.safe(t.name)}(${fields.join(', ')});\n`
      }
    })
    if (stage === 'fragment') {
      // WGSL's fragment position has its origin at the top left, like the
      // raster; GL's is bottom left unless redeclared.
      ioDecl.push('layout(origin_upper_left) in vec4 gl_FragCoord;')
      const l = entry.retAttrs.find(a => a.name === 'location')
      const li = l ? this.evalConst(l.args[0]) : 0
      ioDecl.push(`layout(location = ${li}) out vec4 o_loc${li};`)
      this.cur.fragOut = `o_loc${li}`
    }
    if (stage === 'vertex') {
      const st = this.structs.get(entry.rType.name)
      this.cur.vsOuts = st.resolved.map(f => {
        const fb = f.attrs.find(a => a.name === 'builtin')
        if (fb) return { field: f.name, target: 'gl_Position' }
        const l = this.evalConst(f.attrs.find(a => a.name === 'location').args[0])
        ioDecl.push(`layout(location = ${l}) out ${this.decl(f.type, `v_loc${l}`)};`)
        return { field: f.name, target: `v_loc${l}` }
      })
    }
    const body = this.stmts(entry.body, scope, 1)
    const helpers = []
    if (this.needs.has('fmod')) {
      helpers.push('float wg_fmod(float a, float b) { return a - b * trunc(a / b); }')
      for (const n of [2, 3, 4]) {
        helpers.push(`vec${n} wg_fmod(vec${n} a, vec${n} b) { return a - b * trunc(a / b); }`)
        helpers.push(`vec${n} wg_fmod(vec${n} a, float b) { return a - b * trunc(a / b); }`)
      }
    }
    if (this.needs.has('imod')) {
      helpers.push('int wg_imod(int a, int b) { return a - b * (a / b); }')
      for (const n of [2, 3, 4]) helpers.push(`ivec${n} wg_imod(ivec${n} a, ivec${n} b) { return a - b * (a / b); }`)
    }
    if (this.needs.has('f2u')) {
      helpers.push('uint wg_f2u(float x) { return uint(clamp(x, 0.0, 4294967040.0)); }')
      for (const n of [2, 3, 4]) helpers.push(`uvec${n} wg_f2u(vec${n} x) { return uvec${n}(clamp(x, vec${n}(0.0), vec${n}(4294967040.0))); }`)
    }
    if (this.needs.has('f2i')) {
      helpers.push('int wg_f2i(float x) { return int(clamp(x, -2147483648.0, 2147483520.0)); }')
      for (const n of [2, 3, 4]) helpers.push(`ivec${n} wg_f2i(vec${n} x) { return ivec${n}(clamp(x, vec${n}(-2147483648.0), vec${n}(2147483520.0))); }`)
    }
    const src = [
      '#version 450 core',
      head,
      ...structOut,
      ...constOut,
      ...resDecl,
      ...ioDecl,
      ...helpers,
      ...this.protos,
      ...fnOut,
      `void main() {\n${pre}${body}}`,
    ].join('\n')
    return { stage, src, bindings, workgroupSize: stage === 'compute' ? this.workgroupSize : null }
  }
}

function litValue(tok) {
  if (tok.hex) return parseInt(tok.v, 16)
  return Number(tok.v.endsWith('.') ? tok.v + '0' : tok.v)
}

// ───────────────────────────── CLI ─────────────────────────────
// node wgsl2glsl.mjs prelude.wgsl shader.wgsl [entry]  -> prints GLSL
if (import.meta.url === `file://${process.argv[1]}`) {
  const { readFileSync } = await import('node:fs')
  const [pre, file, entry] = process.argv.slice(2)
  const src = readFileSync(pre, 'utf8') + '\n' + readFileSync(file, 'utf8')
  const res = translate(src)
  const k = entry ?? Object.keys(res)[0]
  process.stdout.write(res[k].src + '\n')
  process.stderr.write(JSON.stringify(res[k].bindings) + '\n')
}
