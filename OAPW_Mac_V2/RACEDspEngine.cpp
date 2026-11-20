#include "RACEDspEngine.h"
#include <cmath>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------
// RACE mit echter Rekursion (wie RPi V21.1)
//   oL[n] = iL[n] - a/(1+p) * ( oR[n-d] - p * oL[n-d] )
//   oR[n] = iR[n] - a/(1+p) * ( oL[n-d] - p * oR[n-d] )
// Pole: Summe a*(1-p)/(1+p), Differenz a. Stabil fuer a < 1 und p >= 0.
//
// Unterschied zum RPi: d ist hier gebrochen (Hermite). Der Mac laeuft meist mit
// 44,1/48 kHz, 68 us sind dort nur 3,26 Samples. |H| des Interpolators ist <= 1,
// die Stabilitaetsgrenze aendert sich dadurch nicht. Minimum 2 Samples, damit die
// Interpolation nur bereits geschriebene Werte liest.
// ---------------------------------------------------------
static constexpr double kMaxRaceFeedback   = 0.95;
static constexpr double kDenormalGuard     = 1e-25;
static constexpr double kMinFilterFreqHz   = 20.0;
static constexpr double kMaxFreqFraction   = 0.45;
static constexpr double kMinCrossoverGapHz = 100.0;
static constexpr double kMinDelaySamples   = 2.0;

static double blackmanHarris(int n, int length) {
    const double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    const double arg = (2.0 * M_PI * n) / (length - 1);
    return a0 - a1 * std::cos(arg) + a2 * std::cos(2.0 * arg) - a3 * std::cos(3.0 * arg);
}

// ========================================================================
// FFT fuer den graphischen Analyzer
// ========================================================================
static void computeFFT(std::vector<std::complex<float>>& data) {
    size_t n = data.size();
    if (n <= 1) return;
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
        std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t j = 0; j < len / 2; j++) {
                std::complex<float> u = data[i + j];
                std::complex<float> v = data[i + j + len / 2] * w;
                data[i + j] = u + v;
                data[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// ========================================================================
// Ringpuffer (Aufnahme)
// ========================================================================
LockFreeRingBuffer::LockFreeRingBuffer(size_t cap) : head(0), tail(0), capacity(cap) {
    buffer.resize(capacity, 0.0f);
}

void LockFreeRingBuffer::push(const float* data, size_t numFrames) {
    size_t next_tail = tail.load(std::memory_order_relaxed);
    for (size_t i = 0; i < numFrames; ++i) {
        size_t next_plus_1 = (next_tail + 1) % capacity;
        if (next_plus_1 == head.load(std::memory_order_acquire)) break;
        buffer[next_tail] = data[i];
        next_tail = next_plus_1;
    }
    tail.store(next_tail, std::memory_order_release);
}

size_t LockFreeRingBuffer::pop(float* outBuffer, size_t maxSamples) {
    size_t current_head = head.load(std::memory_order_relaxed);
    size_t current_tail = tail.load(std::memory_order_acquire);
    size_t samplesRead = 0;
    while (current_head != current_tail && samplesRead < maxSamples) {
        outBuffer[samplesRead++] = buffer[current_head];
        current_head = (current_head + 1) % capacity;
    }
    head.store(current_head, std::memory_order_release);
    return samplesRead;
}

size_t LockFreeRingBuffer::available() const {
    size_t h = head.load(std::memory_order_acquire);
    size_t t = tail.load(std::memory_order_acquire);
    return (t >= h) ? t - h : capacity - h + t;
}

size_t LockFreeRingBuffer::freeSpace() const { return capacity - 1 - available(); }

void LockFreeRingBuffer::clear() {
    head.store(0, std::memory_order_release);
    tail.store(0, std::memory_order_release);
}

// ========================================================================
// FirCrossover
// ========================================================================
FirCrossover::FirCrossover(int sr) : sampleRate(sr) {
    inAudio = fftw_alloc_real(kFft);
    inSpec  = fftw_alloc_complex(kBins);
    planForward = fftw_plan_dft_r2c_1d(kFft, inAudio, inSpec, FFTW_ESTIMATE);
    for (int b = 0; b < 3; ++b) {
        outTime[b]    = fftw_alloc_real(kFft);
        filterIr[b]   = fftw_alloc_real(kFft);
        filterSpec[b] = fftw_alloc_complex(kBins);
        mulSpec[b]    = fftw_alloc_complex(kBins);
        planInverse[b] = fftw_plan_dft_c2r_1d(kFft, mulSpec[b], outTime[b], FFTW_ESTIMATE);
        planFilter[b]  = fftw_plan_dft_r2c_1d(kFft, filterIr[b], filterSpec[b], FFTW_ESTIMATE);
    }
    overlap.assign(kOverlap, 0.0);
}

FirCrossover::~FirCrossover() {
    fftw_destroy_plan(planForward);
    fftw_free(inAudio);
    fftw_free(inSpec);
    for (int b = 0; b < 3; ++b) {
        fftw_destroy_plan(planInverse[b]);
        fftw_destroy_plan(planFilter[b]);
        fftw_free(outTime[b]);
        fftw_free(filterIr[b]);
        fftw_free(filterSpec[b]);
        fftw_free(mulSpec[b]);
    }
}

void FirCrossover::resetState() { std::fill(overlap.begin(), overlap.end(), 0.0); }

void FirCrossover::generateLowpass(double* ir, double cutoff) {
    const double fc = cutoff / sampleRate;
    const int center = (kFirLen - 1) / 2;
    double sum = 0.0;
    for (int i = 0; i < kFirLen; i++) {
        if (i == center) ir[i] = 2.0 * M_PI * fc;
        else ir[i] = std::sin(2.0 * M_PI * fc * (i - center)) / (i - center);
        ir[i] *= blackmanHarris(i, kFirLen);
        sum += ir[i];
    }
    for (int i = 0; i < kFirLen; i++) ir[i] /= sum;
    for (int i = kFirLen; i < kFft; i++) ir[i] = 0.0;
}

void FirCrossover::updateFilters(double freqLow, double freqHigh) {
    const int center = (kFirLen - 1) / 2;
    generateLowpass(filterIr[0], freqLow);
    generateLowpass(filterIr[2], freqHigh);
    for (int i = 0; i < kFirLen; i++) filterIr[2][i] = -filterIr[2][i];
    filterIr[2][center] += 1.0;
    for (int i = 0; i < kFft; i++) {
        const double dirac = (i == center) ? 1.0 : 0.0;
        filterIr[1][i] = dirac - filterIr[0][i] - filterIr[2][i];
    }
    for (int b = 0; b < 3; ++b) fftw_execute(planFilter[b]);
}

void FirCrossover::processBlock(const double* incoming, double* outLow, double* outMid, double* outHigh,
                                const fftw_complex* eqSpectrum) {
    std::memcpy(inAudio, overlap.data(), kOverlap * sizeof(double));
    std::memcpy(inAudio + kOverlap, incoming, kBlock * sizeof(double));
    std::fill(inAudio + kOverlap + kBlock, inAudio + kFft, 0.0);

    std::memmove(overlap.data(), overlap.data() + kBlock, (kOverlap - kBlock) * sizeof(double));
    std::memcpy(overlap.data() + (kOverlap - kBlock), incoming, kBlock * sizeof(double));

    fftw_execute(planForward);

    for (int i = 0; i < kBins; i++) {
        const double ar = inSpec[i][0], ai = inSpec[i][1];
        const double er = eqSpectrum[i][0], ei = eqSpectrum[i][1];
        const double xr = ar * er - ai * ei;
        const double xi = ar * ei + ai * er;
        for (int b = 0; b < 3; ++b) {
            const double fr = filterSpec[b][i][0], fi = filterSpec[b][i][1];
            mulSpec[b][i][0] = xr * fr - xi * fi;
            mulSpec[b][i][1] = xr * fi + xi * fr;
        }
    }

    for (int b = 0; b < 3; ++b) fftw_execute(planInverse[b]);

    const double norm = 1.0 / kFft;
    for (int i = 0; i < kBlock; i++) {
        outLow[i]  = outTime[0][kOverlap + i] * norm;
        outMid[i]  = outTime[1][kOverlap + i] * norm;
        outHigh[i] = outTime[2][kOverlap + i] * norm;
    }
}

// ========================================================================
// RACEDspEngine
// ========================================================================
static int sanitizeRate(int sr) { return (sr >= 8000 && sr <= 768000) ? sr : 48000; }

RACEDspEngine::RACEDspEngine(double initialDelayUs, double initialAttenuationDb, double initialCenterP,
                             bool initialFreqLimit, int sr)
    : sampleRate(sanitizeRate(sr)),
      crossoverL(sanitizeRate(sr)),
      crossoverR(sanitizeRate(sr)),
      drift(2, static_cast<double>(sanitizeRate(sr)))
{
    const size_t L = FirCrossover::kBlock;
    lowL.assign(L, 0.0); midL.assign(L, 0.0); highL.assign(L, 0.0);
    lowR.assign(L, 0.0); midR.assign(L, 0.0); highR.assign(L, 0.0);
    inputFifoL.reserve(L * 3); inputFifoR.reserve(L * 3);
    outputFifoL.reserve(L * 3); outputFifoR.reserve(L * 3);

    delayBufferL.assign(kDelayBufferSize, 0.0);
    delayBufferR.assign(kDelayBufferSize, 0.0);
    asymBufferL.assign(kDelayBufferSize, 0.0);
    spectrumBuffer.assign(kSpectrumSize, 0.0f);
    previousBands.assign(32, 0.0f);
    eqMagnitude.assign(FirCrossover::kBins, 1.0);

    recordQueueL = new LockFreeRingBuffer(1920000);
    recordQueueR = new LockFreeRingBuffer(1920000);

    eqDesignSpec = fftw_alloc_complex(FirCrossover::kBins);
    eqDesignTime = fftw_alloc_real(FirCrossover::kFft);
    eqIrTime     = fftw_alloc_real(FirCrossover::kFft);
    eqSpec       = fftw_alloc_complex(FirCrossover::kBins);
    flatSpec     = fftw_alloc_complex(FirCrossover::kBins);
    planEqInverse = fftw_plan_dft_c2r_1d(FirCrossover::kFft, eqDesignSpec, eqDesignTime, FFTW_ESTIMATE);
    planEqForward = fftw_plan_dft_r2c_1d(FirCrossover::kFft, eqIrTime, eqSpec, FFTW_ESTIMATE);

    // Reine Verzoegerung um die EQ-Mitte: EQ an/aus aendert die Laufzeit nicht.
    const double center = (FirCrossover::kEqLen - 1) / 2.0;
    for (int i = 0; i < FirCrossover::kBins; i++) {
        const double phase = -2.0 * M_PI * i * center / FirCrossover::kFft;
        flatSpec[i][0] = std::cos(phase);
        flatSpec[i][1] = std::sin(phase);
    }

    updateVolumeStepsLocked();
    updateCrossoverLocked(currentFreqLow, currentFreqHigh);
    updateFirEqLocked();
    setParameters(initialDelayUs, initialAttenuationDb, initialCenterP, initialFreqLimit);
    setAsymmetry(false, 0.0, 0.0);
}

RACEDspEngine::~RACEDspEngine() {
    stopRecording();
    fftw_destroy_plan(planEqInverse);
    fftw_destroy_plan(planEqForward);
    fftw_free(eqDesignSpec);
    fftw_free(eqDesignTime);
    fftw_free(eqIrTime);
    fftw_free(eqSpec);
    fftw_free(flatSpec);
    delete recordQueueL;
    delete recordQueueR;
}

void RACEDspEngine::updateVolumeStepsLocked() {
    // Hoch max. 0,5 pro Sekunde, runter in 50 ms (wie RPi)
    volumeStepUp   = 0.5 / static_cast<double>(sampleRate);
    volumeStepDown = 20.0 / static_cast<double>(sampleRate);
}

void RACEDspEngine::updateDelayLocked() {
    double d = delayUs * static_cast<double>(sampleRate) / 1000000.0;
    const double maxD = static_cast<double>(kDelayBufferSize - 4);
    if (!(d >= kMinDelaySamples)) d = kMinDelaySamples;
    if (d > maxD) d = maxD;
    delaySamples = d;
}

void RACEDspEngine::updateAsymmetryLocked() {
    double d = asymDelayMs * (static_cast<double>(sampleRate) / 1000.0);
    const double maxD = static_cast<double>(kDelayBufferSize - 4);
    if (!(d >= kMinDelaySamples)) d = kMinDelaySamples;
    if (d > maxD) d = maxD;
    asymDelaySamples = d;
}

void RACEDspEngine::updateCrossoverLocked(double freqLow, double freqHigh) {
    const double nyqLimit = kMaxFreqFraction * static_cast<double>(sampleRate);
    if (!std::isfinite(freqLow))  freqLow  = currentFreqLow;
    if (!std::isfinite(freqHigh)) freqHigh = currentFreqHigh;
    freqHigh = std::clamp(freqHigh, kMinFilterFreqHz + kMinCrossoverGapHz, nyqLimit);
    freqLow  = std::clamp(freqLow, kMinFilterFreqHz, freqHigh - kMinCrossoverGapHz);
    currentFreqLow = freqLow;
    currentFreqHigh = freqHigh;
    crossoverL.updateFilters(freqLow, freqHigh);
    crossoverR.updateFilters(freqLow, freqHigh);
}

void RACEDspEngine::updateFirCrossover(double freqLow, double freqHigh) {
    std::lock_guard<std::mutex> lock(dspMutex);
    updateCrossoverLocked(freqLow, freqHigh);
}

void RACEDspEngine::resetStreamLocked() {
    crossoverL.resetState();
    crossoverR.resetState();
    inputFifoL.clear(); inputFifoR.clear();
    outputFifoL.clear(); outputFifoR.clear();
    std::fill(delayBufferL.begin(), delayBufferL.end(), 0.0);
    std::fill(delayBufferR.begin(), delayBufferR.end(), 0.0);
    std::fill(asymBufferL.begin(), asymBufferL.end(), 0.0);
    writeIndex = 0;
    asymWriteIndex = 0;
    volumeRamp = 0.0;
    rampHoldFrames = kFilterDelayFrames;
    driftFifo.clear();
    drift.reset();
    driftStarted = false;
    pendingFrames.store(0);
    generation.fetch_add(1);
}

void RACEDspEngine::resetStream() {
    std::lock_guard<std::mutex> lock(dspMutex);
    resetStreamLocked();
}

void RACEDspEngine::setSampleRate(int sr) {
    if (sr < 8000 || sr > 768000) return;   // z.B. 0 Hz ohne Eingabegeraet
    std::lock_guard<std::mutex> lock(dspMutex);
    if (sr == sampleRate) return;
    sampleRate = sr;
    crossoverL.setSampleRate(sr);
    crossoverR.setSampleRate(sr);
    updateCrossoverLocked(currentFreqLow, currentFreqHigh);
    updateFirEqLocked();
    updateDelayLocked();
    updateAsymmetryLocked();
    updateVolumeStepsLocked();
    drift = DriftCompensator(2, static_cast<double>(sr));
    resetStreamLocked();
}

void RACEDspEngine::setParameters(double newDelayUs, double newAttenuationDb, double newCenterP, bool newFreqLimit) {
    std::lock_guard<std::mutex> lock(dspMutex);
    if (!std::isfinite(newDelayUs)) newDelayUs = 68.0;
    if (!std::isfinite(newAttenuationDb)) newAttenuationDb = -2.3;
    if (!std::isfinite(newCenterP)) newCenterP = 0.1;
    delayUs = newDelayUs;
    attenuationDb = newAttenuationDb;
    attenuationLinear = std::pow(10.0, -std::abs(attenuationDb) / 20.0);
    if (!(attenuationLinear < kMaxRaceFeedback)) attenuationLinear = kMaxRaceFeedback;
    if (!(newCenterP >= 0.0)) newCenterP = 0.0;
    if (newCenterP > 1.0) newCenterP = 1.0;
    centerP = newCenterP;
    freqLimitRACE = newFreqLimit;
    updateDelayLocked();
}

void RACEDspEngine::setAsymmetry(bool enabled, double delayMs, double attenuationDb_) {
    std::lock_guard<std::mutex> lock(dspMutex);
    asymEnabled = enabled;
    asymDelayMs = std::isfinite(delayMs) ? delayMs : 0.0;
    asymGainL = std::isfinite(attenuationDb_) ? std::pow(10.0, attenuationDb_ / 20.0) : 1.0;
    updateAsymmetryLocked();
}

void RACEDspEngine::setVolume(double newVolume) {
    if (!std::isfinite(newVolume)) return;
    std::lock_guard<std::mutex> lock(dspMutex);
    volume = std::clamp(newVolume, 0.0, 1.0);
}

void RACEDspEngine::setMakeupGain(double gainDb) {
    if (!std::isfinite(gainDb)) return;
    gainDb = std::clamp(gainDb, -24.0, kMaxMakeupGainDb);
    std::lock_guard<std::mutex> lock(dspMutex);
    makeupGainDb = gainDb;
    makeupGainLinear = std::pow(10.0, makeupGainDb / 20.0);
}

void RACEDspEngine::setRaceEnabled(bool enabled) { std::lock_guard<std::mutex> lock(dspMutex); raceEnabled = enabled; }
void RACEDspEngine::setLowBypassEnabled(bool enabled) { std::lock_guard<std::mutex> lock(dspMutex); lowBypassEnabled = enabled; }
void RACEDspEngine::setCenterLevel(double level) { std::lock_guard<std::mutex> lock(dspMutex); centerLevel = std::isfinite(level) ? level : 0.0; }
void RACEDspEngine::setEqEnabled(bool enabled) { std::lock_guard<std::mutex> lock(dspMutex); eqEnabled = enabled; }

void RACEDspEngine::setEqBand(int b, double f, double q, double g) {
    if (!std::isfinite(f) || !std::isfinite(q) || !std::isfinite(g)) return;
    if (b < 0 || b >= 3) return;
    std::lock_guard<std::mutex> lock(dspMutex);
    eqBands[b].freq = std::clamp(f, kMinFilterFreqHz, kMaxFreqFraction * static_cast<double>(sampleRate));
    eqBands[b].q = std::clamp(q, 0.1, 20.0);
    eqBands[b].gain = std::clamp(g, -24.0, 24.0);
    updateFirEqLocked();
}

inline double RACEDspEngine::hermite(double fraction, double y0, double y1, double y2, double y3) const {
    const double c0 = y1;
    const double c1 = 0.5 * (y2 - y0);
    const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return c0 + fraction * (c1 + fraction * (c2 + fraction * c3));
}

void RACEDspEngine::processSamples(float* left, float* right, int numFrames) {
    if (!left || !right || numFrames <= 0) return;
    std::lock_guard<std::mutex> lock(dspMutex);
    processLocked(left, right, numFrames);
}

void RACEDspEngine::processLocked(float* left, float* right, int numFrames) {
    const int L = FirCrossover::kBlock;
    const int bufferSize = kDelayBufferSize;

    for (int i = 0; i < numFrames; i++) {
        double inL = std::isfinite(left[i])  ? static_cast<double>(left[i])  : 0.0;
        double inR = std::isfinite(right[i]) ? static_cast<double>(right[i]) : 0.0;
        if (asymEnabled) {
            const int dInt = static_cast<int>(asymDelaySamples);
            const double frac = asymDelaySamples - dInt;
            const int w = asymWriteIndex;
            const double delayed = hermite(frac,
                asymBufferL[(w - dInt + 1 + bufferSize) % bufferSize],
                asymBufferL[(w - dInt + bufferSize) % bufferSize],
                asymBufferL[(w - dInt - 1 + bufferSize) % bufferSize],
                asymBufferL[(w - dInt - 2 + bufferSize) % bufferSize]);
            asymBufferL[w] = inL;
            asymWriteIndex = (w + 1) % bufferSize;
            inL = delayed * asymGainL;
        }
        inputFifoL.push_back(inL);
        inputFifoR.push_back(inR);
    }

    while (inputFifoL.size() >= static_cast<size_t>(L)) {
        const fftw_complex* eq = eqEnabled ? eqSpec : flatSpec;
        crossoverL.processBlock(inputFifoL.data(), lowL.data(), midL.data(), highL.data(), eq);
        crossoverR.processBlock(inputFifoR.data(), lowR.data(), midR.data(), highR.data(), eq);
        inputFifoL.erase(inputFifoL.begin(), inputFifoL.begin() + L);
        inputFifoR.erase(inputFifoR.begin(), inputFifoR.begin() + L);

        const int dInt = static_cast<int>(delaySamples);
        const double frac = delaySamples - dInt;
        const double feedback = attenuationLinear / (1.0 + centerP);

        for (int k = 0; k < L; k++) {
            double bypassL = 0.0, bypassR = 0.0, midInL, midInR;
            if (freqLimitRACE) {
                if (lowBypassEnabled) {
                    bypassL = lowL[k] + highL[k];
                    bypassR = lowR[k] + highR[k];
                    midInL = midL[k];
                    midInR = midR[k];
                } else {
                    bypassL = highL[k];
                    bypassR = highR[k];
                    midInL = lowL[k] + midL[k];
                    midInR = lowR[k] + midR[k];
                }
            } else {
                midInL = lowL[k] + midL[k] + highL[k];
                midInR = lowR[k] + midR[k] + highR[k];
            }

            double crossTalkL = 0.0, crossTalkR = 0.0;
            if (raceEnabled) {
                const int i0 = (writeIndex - dInt + 1 + bufferSize) % bufferSize;
                const int i1 = (writeIndex - dInt + bufferSize) % bufferSize;
                const int i2 = (writeIndex - dInt - 1 + bufferSize) % bufferSize;
                const int i3 = (writeIndex - dInt - 2 + bufferSize) % bufferSize;
                const double dOutL = hermite(frac, delayBufferL[i0], delayBufferL[i1], delayBufferL[i2], delayBufferL[i3]);
                const double dOutR = hermite(frac, delayBufferR[i0], delayBufferR[i1], delayBufferR[i2], delayBufferR[i3]);
                crossTalkL = feedback * (dOutR - centerP * dOutL);
                crossTalkR = feedback * (dOutL - centerP * dOutR);
            }

            double raceL = midInL - crossTalkL;
            double raceR = midInR - crossTalkR;

            if (!std::isfinite(raceL) || !std::isfinite(raceR)) {
                std::fill(delayBufferL.begin(), delayBufferL.end(), 0.0);
                std::fill(delayBufferR.begin(), delayBufferR.end(), 0.0);
                raceL = 0.0;
                raceR = 0.0;
            }

            delayBufferL[writeIndex] = (std::fabs(raceL) < kDenormalGuard) ? 0.0 : raceL;
            delayBufferR[writeIndex] = (std::fabs(raceR) < kDenormalGuard) ? 0.0 : raceR;
            writeIndex = (writeIndex + 1) % bufferSize;

            const double center = (midInL + midInR) * 0.5 * centerLevel;
            outputFifoL.push_back((bypassL + raceL + center) * makeupGainLinear);
            outputFifoR.push_back((bypassR + raceR + center) * makeupGainLinear);
        }
    }

    // Lautstaerke, Begrenzung und Analyzer erst beim Auslesen: Regler wirken sofort.
    const int available = std::min(numFrames, static_cast<int>(outputFifoL.size()));
    for (int i = 0; i < available; i++) {
        // Die Rampe startet erst mit dem ersten echten Audio, nicht schon in der Filterlaufzeit
        if (rampHoldFrames > 0) {
            --rampHoldFrames;
        } else if (volumeRamp < volume) {
            volumeRamp = std::min(volume, volumeRamp + volumeStepUp);
        } else if (volumeRamp > volume) {
            volumeRamp = std::max(volume, volumeRamp - volumeStepDown);
        }

        const double outL = std::clamp(outputFifoL[i] * volumeRamp, -1.0, 1.0);
        const double outR = std::clamp(outputFifoR[i] * volumeRamp, -1.0, 1.0);
        left[i]  = static_cast<float>(outL);
        right[i] = static_cast<float>(outR);
        spectrumBuffer[spectrumIndex] = static_cast<float>((outL + outR) * 0.5);
        spectrumIndex = (spectrumIndex + 1) % kSpectrumSize;
    }
    for (int i = available; i < numFrames; i++) {
        left[i] = 0.0f;
        right[i] = 0.0f;
    }
    if (available > 0) {
        outputFifoL.erase(outputFifoL.begin(), outputFifoL.begin() + available);
        outputFifoR.erase(outputFifoR.begin(), outputFifoR.begin() + available);
    }
}

// ========================================================================
// Taktausgleich
// ========================================================================
void RACEDspEngine::pushInput(const float* left, const float* right, int numFrames) {
    if (!left || !right || numFrames <= 0) return;
    std::lock_guard<std::mutex> lock(dspMutex);
    for (int i = 0; i < numFrames; i++) {
        driftFifo.push_back(left[i]);
        driftFifo.push_back(right[i]);
    }
    // Notbremse, falls laengere Zeit niemand abholt: aelteste Daten verwerfen
    const size_t maxSamples = static_cast<size_t>(sampleRate) * 4 * 2;
    if (driftFifo.size() > maxSamples) {
        driftFifo.erase(driftFifo.begin(), driftFifo.end() - static_cast<long>(maxSamples));
    }
}

int RACEDspEngine::pullOutput(float* left, float* right, int numFrames) {
    if (!left || !right || numFrames <= 0) return 0;
    std::lock_guard<std::mutex> lock(dspMutex);

    const long fifoFrames = static_cast<long>(driftFifo.size() / 2);
    const long target = targetFramesLocked();

    if (driftStarted && pendingFrames.load() == 0) {
        // Player ist leergelaufen: neu vorpuffern statt dauerhaft zu stottern
        driftStarted = false;
        drift.reset();
    }
    if (!driftStarted) {
        if (fifoFrames < target) return 0;
        driftStarted = true;
    }

    const size_t need = drift.framesNeeded(static_cast<size_t>(numFrames));
    if (static_cast<size_t>(fifoFrames) < need) return 0;

    driftOut.resize(static_cast<size_t>(numFrames) * 2);
    drift.process(driftFifo.data(), need, driftOut.data(), static_cast<size_t>(numFrames));
    driftFifo.erase(driftFifo.begin(), driftFifo.begin() + static_cast<long>(need * 2));

    for (int i = 0; i < numFrames; i++) {
        left[i]  = driftOut[static_cast<size_t>(i) * 2];
        right[i] = driftOut[static_cast<size_t>(i) * 2 + 1];
    }
    processLocked(left, right, numFrames);

    const long pending = pendingFrames.fetch_add(numFrames) + numFrames;
    if (driftEnabled) {
        const double fill = static_cast<double>(pending) + static_cast<double>(driftFifo.size() / 2);
        drift.control(fill, static_cast<double>(target),
                      static_cast<double>(numFrames) / static_cast<double>(sampleRate));
    }
    return numFrames;
}

void RACEDspEngine::outputConsumed(int numFrames, int gen) {
    if (gen != generation.load()) return;   // Rueckmeldung aus der Zeit vor einem Neustart
    long prev = pendingFrames.fetch_sub(numFrames);
    if (prev < numFrames) pendingFrames.store(0);
}

void RACEDspEngine::setDriftEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(dspMutex);
    driftEnabled = enabled;
    if (!enabled) drift.setPpmForTest(0.0);
}

double RACEDspEngine::driftPpm() {
    std::lock_guard<std::mutex> lock(dspMutex);
    return drift.ppm();
}

long RACEDspEngine::bufferedFrames() {
    std::lock_guard<std::mutex> lock(dspMutex);
    return pendingFrames.load() + static_cast<long>(driftFifo.size() / 2);
}

// ========================================================================
// EQ: Biquad-Betragskurve -> phasenlinearer FIR
// ========================================================================
void RACEDspEngine::updateFirEqLocked() {
    const int bins = FirCrossover::kBins;
    const int N = FirCrossover::kFft;
    std::fill(eqMagnitude.begin(), eqMagnitude.end(), 1.0);

    for (int b = 0; b < 3; ++b) {
        if (std::abs(eqBands[b].gain) < 0.01) continue;
        const double A = std::pow(10.0, eqBands[b].gain / 40.0);
        const double w0 = 2.0 * M_PI * eqBands[b].freq / sampleRate;
        const double alpha = std::sin(w0) / (2.0 * eqBands[b].q);
        const double a0 = 1.0 + alpha / A;
        const double b0 = (1.0 + alpha * A) / a0;
        const double b1 = (-2.0 * std::cos(w0)) / a0;
        const double b2 = (1.0 - alpha * A) / a0;
        const double a1 = b1;
        const double a2 = (1.0 - alpha / A) / a0;

        for (int i = 0; i < bins; ++i) {
            const double theta = M_PI * static_cast<double>(i) / static_cast<double>(bins - 1);
            const double c1 = std::cos(theta), c2 = std::cos(2.0 * theta);
            const double s1 = std::sin(theta), s2 = std::sin(2.0 * theta);
            const double nr = b0 + b1 * c1 + b2 * c2;
            const double ni = -(b1 * s1 + b2 * s2);
            const double dr = 1.0 + a1 * c1 + a2 * c2;
            const double di = -(a1 * s1 + a2 * s2);
            eqMagnitude[i] *= std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
        }
    }

    for (int i = 0; i < bins; ++i) {
        eqDesignSpec[i][0] = eqMagnitude[i];
        eqDesignSpec[i][1] = 0.0;
    }
    fftw_execute(planEqInverse);

    const int M = FirCrossover::kEqLen;
    const int center = (M - 1) / 2;
    const double norm = 1.0 / N;
    for (int i = 0; i < M; ++i) {
        eqIrTime[i] = eqDesignTime[(i - center + N) % N] * norm * blackmanHarris(i, M);
    }
    std::fill(eqIrTime + M, eqIrTime + N, 0.0);
    fftw_execute(planEqForward);
}

void RACEDspEngine::getEqCurve(float* outBuffer, int numPoints) {
    if (numPoints <= 1 || !outBuffer) return;
    std::lock_guard<std::mutex> lock(dspMutex);
    const double logMin = std::log10(20.0);
    const double logMax = std::log10(20000.0);
    const double hzPerBin = static_cast<double>(sampleRate) / static_cast<double>(FirCrossover::kFft);
    for (int i = 0; i < numPoints; i++) {
        const double freq = std::pow(10.0, logMin + (static_cast<double>(i) / (numPoints - 1)) * (logMax - logMin));
        int bin = static_cast<int>(freq / hzPerBin);
        bin = std::clamp(bin, 1, FirCrossover::kBins - 1);
        outBuffer[i] = static_cast<float>(20.0 * std::log10(eqMagnitude[bin] + 1e-6));
    }
}

// ========================================================================
// Analyzer (zeigt jetzt das Ausgangssignal, wie der RPi)
// ========================================================================
std::vector<float> RACEDspEngine::getSpectrumBands() {
    std::vector<float> audioData(kSpectrumSize, 0.0f);
    float rate;
    {
        std::lock_guard<std::mutex> lock(dspMutex);
        for (int i = 0; i < kSpectrumSize; i++) {
            audioData[i] = spectrumBuffer[(spectrumIndex + i) % kSpectrumSize];
        }
        rate = static_cast<float>(sampleRate);
    }

    std::vector<std::complex<float>> complexData(kSpectrumSize);
    for (int i = 0; i < kSpectrumSize; i++) {
        float multiplier = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / 8191.0f));
        complexData[i] = std::complex<float>(audioData[i] * multiplier, 0.0f);
    }
    computeFFT(complexData);

    std::vector<float> bands(32, 0.0f);
    const float logMin = std::log10(20.0f), logMax = std::log10(20000.0f);
    const float hzPerBin = rate / 8192.0f;

    for (int b = 0; b < 32; b++) {
        float freqStart = std::pow(10.0f, logMin + static_cast<float>(b) / 32.0f * (logMax - logMin));
        float freqEnd   = std::pow(10.0f, logMin + static_cast<float>(b + 1) / 32.0f * (logMax - logMin));
        int binStart = static_cast<int>(freqStart / hzPerBin);
        int binEnd   = static_cast<int>(freqEnd / hzPerBin);
        if (binStart < 1) binStart = 1;
        if (binEnd > 4095) binEnd = 4095;
        if (binStart > binEnd) binEnd = binStart;

        float energySum = 0.0f;
        for (int i = binStart; i <= binEnd; i++) {
            float mag = std::abs(complexData[i]) / 16384.0f;
            energySum += mag * mag;
        }
        float db = 20.0f * std::log10(std::sqrt(energySum) + 1e-12f);
        float normalized = std::clamp((db + 85.0f) / 65.0f, 0.0f, 1.0f);

        const float falloffSpeed = 0.025f;
        if (normalized >= previousBands[b]) {
            previousBands[b] = normalized;
        } else {
            previousBands[b] = std::max(0.0f, previousBands[b] - falloffSpeed);
        }
        bands[b] = previousBands[b];
    }
    return bands;
}

// ========================================================================
// Aufnahme (.wav, 32-Bit Float)
// ========================================================================
void RACEDspEngine::writeWavHeader(std::ofstream& file, uint32_t numFrames, uint32_t sr) {
    uint32_t byteRate = sr * 2 * sizeof(float);
    uint32_t dataSize = numFrames * 2 * sizeof(float);
    uint32_t chunkSize = 36 + dataSize;
    uint16_t audioFormat = 3;
    uint16_t numChannels = 2;
    uint16_t blockAlign = numChannels * sizeof(float);
    uint16_t bitsPerSample = 32;
    uint32_t subchunk1Size = 16;

    file.seekp(0);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&chunkSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&sr), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
}

void RACEDspEngine::startRecording(const std::string& path, int sr) {
    if (isRecording.load()) return;
    currentRecordPath = path;
    recordSampleRate = sr;
    recordQueueL->clear();
    recordQueueR->clear();
    isRecording.store(true);
    writerThread = new std::thread(&RACEDspEngine::diskWriterLoop, this);
}

void RACEDspEngine::stopRecording() {
    if (isRecording.load()) {
        isRecording.store(false);
        if (writerThread && writerThread->joinable()) {
            writerThread->join();
            delete writerThread;
            writerThread = nullptr;
        }
    }
}

void RACEDspEngine::diskWriterLoop() {
    std::ofstream outFile(currentRecordPath, std::ios::binary);
    if (!outFile.is_open()) return;

    writeWavHeader(outFile, 0, static_cast<uint32_t>(recordSampleRate));

    // RIFF-Grenze: 4 GiB. Darueber wird nicht weitergeschrieben, die Datei bleibt gueltig.
    const uint64_t maxFrames = (0xFFFFFFFFULL - 36ULL) / (2ULL * sizeof(float));
    uint64_t totalFrames = 0;
    const size_t CHUNK = 4096;
    std::vector<float> bufL(CHUNK), bufR(CHUNK), interleaved(CHUNK * 2);

    auto avail = [this]() { return std::min(recordQueueL->available(), recordQueueR->available()); };

    while (isRecording.load() || avail() > 0) {
        const size_t a = avail();
        if (a > 0) {
            const size_t toRead = std::min(a, CHUNK);
            recordQueueL->pop(bufL.data(), toRead);
            recordQueueR->pop(bufR.data(), toRead);
            if (totalFrames + toRead > maxFrames) continue;
            for (size_t i = 0; i < toRead; ++i) {
                interleaved[i * 2] = bufL[i];
                interleaved[i * 2 + 1] = bufR[i];
            }
            outFile.write(reinterpret_cast<const char*>(interleaved.data()), toRead * 2 * sizeof(float));
            totalFrames += toRead;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    writeWavHeader(outFile, static_cast<uint32_t>(totalFrames), static_cast<uint32_t>(recordSampleRate));
    outFile.close();
}

void RACEDspEngine::enqueueRecordSamples(float* left, float* right, int numFrames) {
    if (!isRecording.load() || !left || !right || numFrames <= 0) return;
    // Beide Kanaele oder keiner: sonst liefen L und R auseinander
    const size_t n = static_cast<size_t>(numFrames);
    if (recordQueueL->freeSpace() < n || recordQueueR->freeSpace() < n) return;
    recordQueueL->push(left, n);
    recordQueueR->push(right, n);
}

// ========================================================================
// C-WRAPPER FUER SWIFT
// ========================================================================
static inline RACEDspEngine* E(void* p) { return static_cast<RACEDspEngine*>(p); }

extern "C" {
    void* createRACEEngine(float dn, float attenuation, float centerP, bool freqLimit, int sampleRate) {
        return new RACEDspEngine(dn, attenuation, centerP, freqLimit, sampleRate);
    }
    void destroyRACEEngine(void* p) { if (p) delete E(p); }
    void wrapper_setSampleRate(void* p, int sr) { if (p) E(p)->setSampleRate(sr); }
    void wrapper_resetStream(void* p) { if (p) E(p)->resetStream(); }
    void wrapper_setCenterLevel(void* p, float level) { if (p) E(p)->setCenterLevel(level); }
    void wrapper_setParameters(void* p, float dn, float attenuation, float centerP, bool freqLimit) { if (p) E(p)->setParameters(dn, attenuation, centerP, freqLimit); }
    void wrapper_setRaceEnabled(void* p, bool enabled) { if (p) E(p)->setRaceEnabled(enabled); }
    void wrapper_setLowBypassEnabled(void* p, bool enabled) { if (p) E(p)->setLowBypassEnabled(enabled); }
    void wrapper_setVolume(void* p, float volume) { if (p) E(p)->setVolume(volume); }
    void wrapper_setMakeupGain(void* p, float gainDb) { if (p) E(p)->setMakeupGain(gainDb); }
    void wrapper_setEqEnabled(void* p, bool enabled) { if (p) E(p)->setEqEnabled(enabled); }
    void wrapper_setEqBand(void* p, int band, float freq, float q, float gain) { if (p) E(p)->setEqBand(band, freq, q, gain); }
    void wrapper_getEqCurve(void* p, float* out, int n) { if (p) E(p)->getEqCurve(out, n); }
    void wrapper_setAsymmetry(void* p, bool enabled, float delayMs, float attenuationDb) { if (p) E(p)->setAsymmetry(enabled, delayMs, attenuationDb); }
    void wrapper_processSamples(void* p, float* l, float* r, int n) { if (p) E(p)->processSamples(l, r, n); }
    void wrapper_getSpectrumBands(void* p, float* out, int numBands) {
        if (!p || !out) return;
        std::vector<float> bands = E(p)->getSpectrumBands();
        int limit = std::min(static_cast<int>(bands.size()), numBands);
        for (int i = 0; i < limit; i++) out[i] = bands[i];
    }
    void wrapper_pushInput(void* p, const float* l, const float* r, int n) { if (p) E(p)->pushInput(l, r, n); }
    int  wrapper_pullOutput(void* p, float* l, float* r, int n) { return p ? E(p)->pullOutput(l, r, n) : 0; }
    void wrapper_outputConsumed(void* p, int n, int gen) { if (p) E(p)->outputConsumed(n, gen); }
    int  wrapper_getStreamGeneration(void* p) { return p ? E(p)->streamGeneration() : 0; }
    void wrapper_setDriftEnabled(void* p, bool enabled) { if (p) E(p)->setDriftEnabled(enabled); }
    double wrapper_getDriftPpm(void* p) { return p ? E(p)->driftPpm() : 0.0; }
    long wrapper_getBufferedFrames(void* p) { return p ? E(p)->bufferedFrames() : 0; }
    void wrapper_startRecording(void* p, const char* path, int sr) { if (p && path) E(p)->startRecording(std::string(path), sr); }
    void wrapper_stopRecording(void* p) { if (p) E(p)->stopRecording(); }
    void wrapper_enqueueRecordSamples(void* p, float* l, float* r, int n) { if (p) E(p)->enqueueRecordSamples(l, r, n); }
    void wrapper_setFirCrossover(void* p, float low, float high) { if (p) E(p)->updateFirCrossover(low, high); }
}
