// Port of the CPU half of Engine.render()/renderFrame() from videoskillet
// src/core/gpu/pipeline.ts (MIT, Colin Diesh). The order of every state update
// and every draw from the dice matches the TypeScript, which is what lets the
// golden test compare the two frame by frame.
#include "chain.h"

#include <cstring>

namespace skillet {

// ── feedgates.ts ──
bool feedFaults(const Controls& c, bool isA) {
  if (isA)
    return c[C_aScramble] > 0 || c[C_aTermination] != 0 || c[C_aNoiseIre] > 0 || c[C_aPolarity] > 0 ||
           c[C_aHumIre] != 0 || c[C_aConnector] > 0 || c[C_aDropoutRate] > 0;
  return c[C_bScramble] > 0 || c[C_bTermination] != 0 || c[C_bNoiseIre] > 0 || c[C_bPolarity] > 0 ||
         c[C_bHumIre] != 0 || c[C_bConnector] > 0 || c[C_bDropoutRate] > 0;
}
bool aFeedOn(const Controls& c) { return feedFaults(c, true) || c[C_aPause] > 0; }
bool bWaveOn(const Controls& c, bool bEnabled) {
  return bEnabled && (c[C_bGenlock] < 0.5 ? c[C_bGain] != 0 || c[C_bRing] != 0 : c[C_bGain] > 0);
}
bool bFeedOn(const Controls& c, bool bEnabled) {
  return bWaveOn(c, bEnabled) && (feedFaults(c, false) || (c[C_bGenlock] < 0.5 && c[C_bPause] > 0));
}
bool bOn(const Controls& c, bool bEnabled) {
  return bEnabled &&
         (c[C_pipMix] != 0 || (c[C_bGenlock] < 0.5 ? c[C_bGain] != 0 || c[C_bRing] != 0 || c[C_aGain] != 1 : c[C_bGain] > 0));
}

SignalChain::SignalChain(uint32_t seed)
    : dice_(seed), rand_([this] { return dice_(); }), lineState_(&rand_), mixState_(&rand_), servo_(&rand_) {
  std::memset(params, 0, sizeof params);
  std::memset(genParams, 0, sizeof genParams);
  std::memset(feedParamsA, 0, sizeof feedParamsA);
  std::memset(feedParamsB, 0, sizeof feedParamsB);
  std::memset(lineParams, 0, sizeof lineParams);
  std::memset(filters, 0, sizeof filters);
}

static bool isFilterKey(int k) {
  for (int i = 0; i < kFilterKeysCount; i++)
    if (kFilterKeys[i] == k) return true;
  return false;
}

bool SignalChain::step(const FrameEnv& env) {
  filtersFresh = false;
  // advanceGlide: walk the resting board toward a morph target
  if (glide.running()) {
    bool done = false;
    if (glide.apply(controls, env.nowMs, done)) filtersDirty_ = true;
  }
  // applyClip: the paperclip overlays its contact point's controls for one
  // frame, lerped from wherever they rest, and they are restored afterwards.
  Controls live = controls;
  if (overlay) overlay(live);
  ClipStep cs;
  if (clip_.step(controls[C_clipHz], controls[C_clipBite], controls[C_clipDwellMs], controls[C_clipChatter],
                 clipPointAt(controls[C_clipPoint]), rand_, cs)) {
    const ClipPointDef& def = kClipPoints[cs.point];
    bool moved = false;
    for (int k = 0; k < kNumControls; k++) {
      for (int i = 0; i < def.count; i++) {
        const ClipPeak& pk = kClipPeaks[def.first + i];
        if (pk.key != k) continue;
        live[k] = controls[k] + (pk.value - controls[k]) * cs.depth;
        if (isFilterKey(k)) moved = true;
      }
    }
    if (moved) filtersDirty_ = true;
  }
  simAcc_ = jmin(simAcc_ + live[C_timeScale], 1);
  if (simAcc_ >= 1) {
    simAcc_ -= 1;
    renderFrameCpu(env, live);
    return true;
  }
  return false;
}

void SignalChain::packFeed(bool isA, const double vals[kNumParams], const Controls& c, const DeckPause& deck,
                           uint8_t out[kParamBytes]) {
  double v[kNumParams];
  std::memcpy(v, vals, sizeof v);
  v[P_gen] = isA ? 101 : 102;
  v[P_srcFrame] = isA ? tapeA_ : tapeB_;
  v[P_scramble] = c[isA ? C_aScramble : C_bScramble];
  v[P_scrambleMode] = c[isA ? C_aScrambleMode : C_bScrambleMode];
  v[P_termination] = c[isA ? C_aTermination : C_bTermination];
  v[P_noiseSigma] = c[isA ? C_aNoiseIre : C_bNoiseIre];
  v[P_polarityFlip] = c[isA ? C_aPolarity : C_bPolarity];
  v[P_humAmp] = c[isA ? C_aHumIre : C_bHumIre];
  v[P_connectorGlitch] = c[isA ? C_aConnector : C_bConnector];
  v[P_connectorMode] = c[isA ? C_aConnectorMode : C_bConnectorMode];
  v[P_dropoutRate] = c[isA ? C_aDropoutRate : C_bDropoutRate];
  v[P_dropoutLen] = c[isA ? C_aDropoutLenUs : C_bDropoutLenUs] * 1e-6 * SAMPLE_RATE;
  v[P_bPause] = deck.pause;
  v[P_bPauseBar] = deck.bar;
  v[P_bShift0] = deck.shift;
  v[P_bRowOff] = deck.row;
  packParams(v, out);
}

void SignalChain::renderFrameCpu(const FrameEnv& env, const Controls& c) {
  // Any writer can move a filter control (a morph, the clip, a host knob), so
  // the bank is redesigned whenever the five values it is built from change.
  for (int i = 0; i < kFilterKeysCount; i++) {
    if (!(c[kFilterKeys[i]] == lastFilterVals_[i])) filtersDirty_ = true;
    lastFilterVals_[i] = c[kFilterKeys[i]];
  }
  if (filtersDirty_) {
    designFilterBank(c, filters);
    filtersDirty_ = false;
    filtersFresh = true;
  }
  const double N = N_SAMPLES;
  // advancePhases
  scPhase_ = jsMod(scPhase_ + loRadPerSample(c[C_scDetuneKHz]) * N, 2 * kPI);
  cfbCarrierPhase_ = jsMod(cfbCarrierPhase_ + loRadPerSample(c[C_cfbCarrierKHz]) * N, 2 * kPI);
  // advanceShuttle
  shuttlePhase_ = advanceCrossings(shuttlePhase_, c[C_shuttleX] - 1);
  if (c[C_aPause] == 0) tapeA_ += 1;
  if (c[C_bPause] == 0) tapeB_ += 1;
  // advanceImpulseTrain
  if (c[C_impulseHz] <= 0) {
    impulseTrainStep_ = 0;
  } else {
    const double fEff = c[C_impulseHz] * (1 + 0.25 * valueNoise((frameCounter_ / 60) * 0.4, 3));
    impulseTrainStep_ = SAMPLE_RATE / fEff;
    impulseTrainPos_ = wrap(impulseTrainPos_ - N, impulseTrainStep_);
  }
  const MixUniforms mixU = mixState_.update(c[C_aPause], c[C_bLineHz], c[C_bDetuneHz], c[C_bRollLps], c[C_bPause],
                                            c[C_wipePos], c[C_wipeRate]);
  // kickServo (the audio half has no input in the plugin)
  const double dShuttle = std::abs(c[C_shuttleX] - lastShuttleX_);
  lastShuttleX_ = c[C_shuttleX];
  if (dShuttle > 0) servo_.kick(jmin(dShuttle, 1));
  track_ = servo_.update(c[C_trackPos], c[C_trackAmt], c[C_trackHunt], c[C_trackKick]);

  UniformEnv ue{};
  ue.frame = frameCounter_;
  ue.canvasW = env.canvasW;
  ue.canvasH = env.canvasH;
  ue.srcAspect = env.srcAspect;
  ue.srcMirror = env.srcMirror;
  ue.tubeTurn = env.tubeTurn;
  ue.srcNoise = env.srcNoise;
  ue.srcNoiseB = env.srcNoiseB;
  ue.srcFrame = tapeA_;
  ue.beamBlank = strobeGate_.step(c[C_strobeHz], c[C_strobeMs], env.nowMs);
  ue.scPhase = scPhase_;
  ue.cfbCarrierPhase = cfbCarrierPhase_;
  ue.audioHit = 0;
  ue.audioLevel = 0;
  ue.impulseTrainPos = impulseTrainPos_;
  ue.impulseTrainStep = impulseTrainStep_;
  ue.shuttlePhase = shuttlePhase_;
  ue.trackPos = track_.pos;
  ue.trackAmt = track_.amt;
  ue.flagUs = track_.flagUs;
  ue.dbgView = 0;
  double vals[kNumParams];
  uniformValues(c, ue, vals);
  vals[P_bShift0] = mixU.bShift0;
  vals[P_bShiftLine] = mixU.bShiftLine;
  vals[P_bPhase0] = mixU.bPhase0;
  vals[P_bPhaseLine] = mixU.bPhaseLine;
  vals[P_bRowOff] = mixU.bRowOff;
  vals[P_wipePos] = mixU.wipePos;
  const RfUniforms rf = rfState_.update(frameCounter_);
  vals[P_rfAdjEps] = rf.rfAdjEps;
  vals[P_rfAdjTau] = rf.rfAdjTau;
  vals[P_rfAdjPhase] = rf.rfAdjPhase;
  vals[P_rfAdjPhaseS] = rf.rfAdjPhaseS;
  vals[P_ingressCps] = rf.ingressCps;
  vals[P_ingressRowCyc] = rf.ingressRowCyc;
  vals[P_ingressPhase] = rf.ingressPhase;
  vals[P_ingressKey] = rf.ingressKey;
  const auto cc = captionState.update(c[C_vbi]);
  vals[P_ccChar0] = cc.first;
  vals[P_ccChar1] = cc.second;
  const SynthUniforms sy = synthState_.update(c[C_synthAHz], c[C_synthBHz]);
  vals[P_synthPhaseA] = sy.phaseA;
  vals[P_synthPerLineA] = sy.perLineA;
  vals[P_synthPerSampleA] = sy.perSampleA;
  vals[P_synthPhaseB] = sy.phaseB;
  vals[P_synthPerLineB] = sy.perLineB;
  vals[P_synthPerSampleB] = sy.perSampleB;
  packParams(vals, params);

  gates.aFeed = aFeedOn(c);
  gates.bChain = bOn(c, env.bEnabled);
  gates.bWave = bWaveOn(c, env.bEnabled);
  gates.bFeed = bFeedOn(c, env.bEnabled);
  if (gates.aFeed) packFeed(true, vals, c, mixU.a, feedParamsA);
  if (gates.bFeed) {
    DeckPause b = mixU.b;
    b.pause = c[C_bGenlock] < 0.5 ? mixU.b.pause : 0;
    packFeed(false, vals, c, b, feedParamsB);
  }
  LineStateControls lc{c[C_tbJitterNs], c[C_tbWowNs], c[C_tbStickNs], c[C_underJitterDeg], c[C_headSwitchShiftUs],
                       track_.amt, track_.pos, c[C_shuttleX] - 1, shuttlePhase_};
  std::memcpy(lineParams[0], lineState_.update(lc, frameCounter_), sizeof lineParams[0]);
  const int gens = static_cast<int>(clampd(jsRound(c[C_dubGens]), 1, MAX_GENS));
  std::memcpy(genParams[0], params, kParamBytes);
  for (int g = 1; g < gens; g++) {
    std::memcpy(genParams[g], params, kParamBytes);
    const uint32_t gu = static_cast<uint32_t>(g);
    std::memcpy(genParams[g] + P_gen * 4, &gu, 4);
    std::memcpy(lineParams[g], lineState_.update(lc, frameCounter_), sizeof lineParams[g]);
  }
  gates.gens = gens;
  gates.composeB = gates.bChain && env.srcNoiseB > 0 && c[C_bPause] == 0;
  gates.chyron = c[C_cgMix] > 0;
  gates.fbComposite = c[C_cfbMix] != 0;
  gates.progSnap = gates.fbComposite && ((c[C_cfbKey] != 0 && c[C_cfbKeyExt] > 0) || c[C_cfbReturn] > 0);
  gates.underDown = c[C_colorUnderMix] > 0;
  gates.enhancer = c[C_enhClampUs] != 0 || c[C_enhDroopUs] > 0 || (c[C_enhPeakMHz] > 0 && c[C_enhPeakBoost] > 0) || c[C_enhSync] > 0;
  gates.vir = c[C_vir] > 0;
  gates.caption = c[C_cc] > 0;
  {
    const double period = c[C_cfbTrail] > 0 ? 2 * std::ceil((c[C_cfbHold] + 1) / 2) : jsRound(c[C_cfbHold]) + 1;
    gates.storePrev = c[C_cfbMix] != 0 && jsMod(frameCounter_, period) == 0;
  }
  gates.crtSrgb = c[C_crtCutoff] > 0 || c[C_crtGamma] != 1;
  frame = static_cast<uint32_t>(frameCounter_);
  frameCounter_ += 1;
}

} // namespace skillet
