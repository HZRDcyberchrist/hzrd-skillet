// Reference: the CPU half of videoskillet's Engine.render()/renderFrame(),
// driven with the real TypeScript classes and a seeded mulberry32, printing
// what each frame packs. The C++ SignalChain must print identical bytes.
//   node ... ref_chain.mjs <presetName> <frames> <seed> [bEnabled]
const VS = (process.env.VS ?? '/home/claude/videoskillet') + '/src'
const { DEFAULT_CONTROLS, CONTROL_KEYS } = await import(`${VS}/core/controls.ts`)
const { PRESET_BY_NAME, presetControls } = await import(`${VS}/ui/presets.ts`)
const { rngFor } = await import(`${VS}/core/rng.ts`)
const { LineState } = await import(`${VS}/core/signal/linestate.ts`)
const { MixState } = await import(`${VS}/core/signal/mixstate.ts`)
const { RfState } = await import(`${VS}/core/signal/rfstate.ts`)
const { SynthState } = await import(`${VS}/core/signal/synthstate.ts`)
const { CaptionState } = await import(`${VS}/core/signal/captionstate.ts`)
const { StrobeGate } = await import(`${VS}/core/signal/strobe.ts`)
const { ClipContact, clipPointAt, clipPointDef } = await import(`${VS}/core/signal/clip.ts`)
const { TrackingServo } = await import(`${VS}/core/signal/servo.ts`)
const { advanceCrossings } = await import(`${VS}/core/signal/crossings.ts`)
const { valueNoise } = await import(`${VS}/core/signal/noise.ts`)
const { wrap, clamp } = await import(`${VS}/core/math.ts`)
const { SAMPLE_RATE, LINES, SAMPLES_PER_LINE } = await import(`${VS}/core/signal/constants.ts`)
const { uniformValues, loRadPerSample } = await import(`${VS}/core/gpu/uniforms.ts`)
const { packParams, patchParams, PARAM_BYTES, GEN_OFFSET } = await import(`${VS}/core/gpu/prelude.ts`)
const { designFilterBank } = await import(`${VS}/core/gpu/filterbank.ts`)
const { FEEDS, aFeedOn, bFeedOn } = await import(`${VS}/core/gpu/feedgates.ts`)

const [name = 'vhs', framesArg = '20', seedArg = '1234', bArg = '1'] = process.argv.slice(2)
const preset = name === 'clean' ? { patch: {} } : PRESET_BY_NAME.get(name)
if (!preset) throw new Error(`no preset ${name}`)
const controls = presetControls(preset.patch)
const bEnabled = bArg === '1'
const rand = rngFor(Number(seedArg))
const N = SAMPLES_PER_LINE * LINES
const lineState = new LineState(rand), mixState = new MixState(rand), rfState = new RfState()
const synthState = new SynthState(), captionState = new CaptionState(), strobe = new StrobeGate()
const clip = new ClipContact(), servo = new TrackingServo(rand)
captionState.setText(process.env.CAPTION ?? '')
let scPhase = 0, cfbCarrierPhase = 0, shuttlePhase = 0, lastShuttleX = 1
let track = { pos: 0.85, amt: 0, flagUs: 0 }
const tapeFrame = { a: 0, b: 0 }
let impulseTrainPos = 0, impulseTrainStep = 0, simAcc = 0, frame = 0, filtersDirty = true
const hex = buf => Buffer.from(buf).toString('hex')
const out = []
const env = { canvasW: 1920, canvasH: 1080, srcAspect: 4 / 3, srcNoise: 0, srcNoiseB: bEnabled ? 1 : 0 }
for (let refresh = 0; refresh < Number(framesArg); refresh++) {
  const nowMs = refresh * 1000 / 60
  const saved = {}
  const step = clip.step({ hz: controls.clipHz, bite: controls.clipBite, dwellMs: controls.clipDwellMs, chatter: controls.clipChatter, point: clipPointAt(controls.clipPoint) }, rand)
  if (step !== null) for (const k of CONTROL_KEYS) { const to = step.peak[k]; if (to === undefined) continue; saved[k] = controls[k]; controls[k] = controls[k] + (to - controls[k]) * step.depth }
  simAcc = Math.min(simAcc + controls.timeScale, 1)
  if (simAcc >= 1) {
    simAcc -= 1
    const c = controls
    if (filtersDirty) { out.push(`filters ${hex(designFilterBank(c).buffer)}`); filtersDirty = false }
    scPhase = (scPhase + loRadPerSample(c.scDetuneKHz) * N) % (2 * Math.PI)
    cfbCarrierPhase = (cfbCarrierPhase + loRadPerSample(c.cfbCarrierKHz) * N) % (2 * Math.PI)
    shuttlePhase = advanceCrossings(shuttlePhase, c.shuttleX - 1)
    if (c.aPause === 0) tapeFrame.a += 1
    if (c.bPause === 0) tapeFrame.b += 1
    if (c.impulseHz <= 0) impulseTrainStep = 0
    else { const fEff = c.impulseHz * (1 + 0.25 * valueNoise((frame / 60) * 0.4, 3)); impulseTrainStep = SAMPLE_RATE / fEff; impulseTrainPos = wrap(impulseTrainPos - N, impulseTrainStep) }
    const mixU = mixState.update({ aPause: c.aPause, bLineHz: c.bLineHz, bDetuneHz: c.bDetuneHz, bRollLps: c.bRollLps, bPause: c.bPause, wipePos: c.wipePos, wipeRateHz: c.wipeRate })
    const dShuttle = Math.abs(c.shuttleX - lastShuttleX); lastShuttleX = c.shuttleX
    if (dShuttle > 0) servo.kick(Math.min(dShuttle, 1))
    track = servo.update({ target: c.trackPos, amt: c.trackAmt, hunt: c.trackHunt, kick: c.trackKick })
    const vals = Object.assign(uniformValues(c, {
      frame, canvasW: env.canvasW, canvasH: env.canvasH, srcAspect: env.srcAspect, srcMirror: 0, tubeTurn: 0,
      srcNoise: env.srcNoise, srcNoiseB: env.srcNoiseB, srcFrame: tapeFrame.a,
      beamBlank: strobe.step({ hz: c.strobeHz, ms: c.strobeMs }, nowMs), scPhase, cfbCarrierPhase,
      audioHit: 0, audioLevel: 0, impulseTrainPos, impulseTrainStep, shuttlePhase,
      trackPos: track.pos, trackAmt: track.amt, flagUs: track.flagUs, dbgView: 0,
    }), mixU, rfState.update(frame), captionState.update({ vbi: c.vbi }), synthState.update({ synthAHz: c.synthAHz, synthBHz: c.synthBHz }))
    const buf = new ArrayBuffer(PARAM_BYTES)
    packParams(vals, buf)
    out.push(`params ${frame} ${hex(buf)}`)
    const feed = (src, deck) => {
      const f = FEEDS[src], fb = new ArrayBuffer(PARAM_BYTES)
      packParams(vals, fb)
      patchParams(fb, { gen: f.gen, srcFrame: tapeFrame[src], scramble: c[f.scramble], scrambleMode: c[f.scrambleMode], termination: c[f.termination], noiseSigma: c[f.noise], polarityFlip: c[f.polarity], humAmp: c[f.hum], connectorGlitch: c[f.connector], connectorMode: c[f.connectorMode], dropoutRate: c[f.dropoutRate], dropoutLen: c[f.dropoutLen] * 1e-6 * SAMPLE_RATE, bPause: deck.pause, bPauseBar: deck.bar, bShift0: deck.shift, bRowOff: deck.row })
      return hex(fb)
    }
    if (aFeedOn(c)) out.push(`feedA ${frame} ${feed('a', mixU.decks.a)}`)
    if (bFeedOn(c, bEnabled)) out.push(`feedB ${frame} ${feed('b', { ...mixU.decks.b, pause: c.bGenlock < 0.5 ? mixU.decks.b.pause : 0 })}`)
    const lc = { tbJitterNs: c.tbJitterNs, tbWowNs: c.tbWowNs, tbStickNs: c.tbStickNs, underJitterDeg: c.underJitterDeg, headSwitchShiftUs: c.headSwitchShiftUs, trackAmt: track.amt, trackPos: track.pos, shuttleBars: c.shuttleX - 1, shuttlePhase }
    out.push(`line0 ${frame} ${hex(lineState.update(lc, frame).buffer)}`)
    const gens = clamp(Math.round(c.dubGens), 1, 4)
    for (let g = 1; g < gens; g++) out.push(`line${g} ${frame} ${hex(lineState.update(lc, frame).buffer)}`)
    frame += 1
  }
  for (const k of Object.keys(saved)) controls[k] = saved[k]
}
process.stdout.write(out.join('\n') + '\n')
