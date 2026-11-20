#ifndef RACEDspEngine_h
#define RACEDspEngine_h

// OAPW_Player Mac: DSP-Kern auf Stand RPi V21.1
// (rekursives RACE, phasenlineare FIR-Weiche, Volume-Rampe, DriftCompensator)

#ifdef __cplusplus
#include <vector>
#include <atomic>
#include <thread>
#include <string>
#include <fstream>
#include <mutex>
#include <complex>
#include <cstdint>
#include <cstddef>

#include <fftw3.h>
#include "DriftCompensator.h"

// --- Lock-Free Ringbuffer (nur noch fuer die Aufnahme) ---
class LockFreeRingBuffer {
public:
    LockFreeRingBuffer(size_t capacity);
    void push(const float* data, size_t numFrames);
    size_t pop(float* outBuffer, size_t maxSamples);
    size_t available() const;
    size_t freeSpace() const;
    void clear();
private:
    std::vector<float> buffer;
    std::atomic<size_t> head;
    std::atomic<size_t> tail;
    size_t capacity;
};

// --- Phasenlineare Dreiband-Weiche mit integriertem EQ (Overlap-Save, 64 Bit) ---
// Gesamt-Impulsantwort: Weiche (M) gefaltet mit EQ (Meq). Die FFT ist so gross, dass
// beide zusammen ohne zirkulaeren Fehler passen.
class FirCrossover {
public:
    static constexpr int kBlock  = 32768;   // Hop
    static constexpr int kFirLen = 32767;   // Weiche
    static constexpr int kEqLen  = 32769;   // EQ (ungerade: ganzzahlige Mitte)
    static constexpr int kFft    = 131072;
    static constexpr int kBins   = kFft / 2 + 1;
    static constexpr int kOverlap = kFirLen + kEqLen - 2;

    explicit FirCrossover(int sampleRate);
    ~FirCrossover();
    FirCrossover(const FirCrossover&) = delete;
    FirCrossover& operator=(const FirCrossover&) = delete;

    void setSampleRate(int sr) { sampleRate = sr; }
    void resetState();
    void updateFilters(double freqLow, double freqHigh);
    void processBlock(const double* incoming, double* outLow, double* outMid, double* outHigh,
                      const fftw_complex* eqSpectrum);

private:
    void generateLowpass(double* ir, double cutoff);

    int sampleRate;
    double* inAudio;
    fftw_complex* inSpec;
    double* outTime[3];
    double* filterIr[3];
    fftw_complex* filterSpec[3];
    fftw_complex* mulSpec[3];
    fftw_plan planForward;
    fftw_plan planInverse[3];
    fftw_plan planFilter[3];
    std::vector<double> overlap;
};

class RACEDspEngine {
public:
    // Sicherheit fuer Endstufen/Lautsprecher (wie RPi)
    static constexpr double kDefaultVolume   = 0.10;
    static constexpr double kMaxMakeupGainDb = 6.0;

    RACEDspEngine(double initialDelayUs, double initialAttenuationDb, double initialCenterP,
                  bool initialFreqLimit, int sampleRate);
    ~RACEDspEngine();

    void setSampleRate(int sr);
    void resetStream();

    void setParameters(double newDelayUs, double newAttenuationDb, double newCenterP, bool newFreqLimit);
    void setVolume(double newVolume);
    void setMakeupGain(double gainDb);
    void setRaceEnabled(bool enabled);
    void setLowBypassEnabled(bool enabled);
    void setCenterLevel(double level);
    void setEqEnabled(bool enabled);
    void setEqBand(int band, double freq, double q, double gainDb);
    void setAsymmetry(bool enabled, double delayMs, double attenuationDb);
    void updateFirCrossover(double freqLow, double freqHigh);

    // Gleich viele Frames rein wie raus, ohne Taktausgleich
    void processSamples(float* leftBuffer, float* rightBuffer, int numFrames);

    // Pfad mit Taktausgleich: Eingangstakt (Tap) -> pushInput, Ausgabe -> pullOutput.
    void pushInput(const float* left, const float* right, int numFrames);
    int  pullOutput(float* left, float* right, int numFrames); // 0 = noch nichts bereit
    void outputConsumed(int numFrames, int generation);        // Player hat Frames verbraucht
    int  streamGeneration() const { return generation.load(); }
    void setDriftEnabled(bool enabled);
    double driftPpm();
    long bufferedFrames();

    std::vector<float> getSpectrumBands();
    void getEqCurve(float* outBuffer, int numPoints);

    void startRecording(const std::string& path, int sampleRate);
    void stopRecording();
    void enqueueRecordSamples(float* leftBuffer, float* rightBuffer, int numFrames);

private:
    static constexpr int kDelayBufferSize = 32768;
    static constexpr int kSpectrumSize = 8192;

    struct EqBandDef { double freq = 1000.0; double q = 0.707; double gain = 0.0; };

    void processLocked(float* left, float* right, int numFrames);
    void resetStreamLocked();
    void updateCrossoverLocked(double freqLow, double freqHigh);
    void updateFirEqLocked();
    void updateDelayLocked();
    void updateAsymmetryLocked();
    void updateVolumeStepsLocked();
    long targetFramesLocked() const { return sampleRate / 2; }

    inline double hermite(double fraction, double y0, double y1, double y2, double y3) const;

    std::mutex dspMutex;

    int sampleRate;
    double delayUs = 68.0, attenuationDb = -2.3, attenuationLinear = 0.767, centerP = 0.1;
    double delaySamples = 3.0;
    bool freqLimitRACE = true;
    bool raceEnabled = true;
    bool lowBypassEnabled = true;  // entspricht dem bisherigen Mac-Verhalten
    bool eqEnabled = false;
    double centerLevel = 0.0;
    double currentFreqLow = 250.0, currentFreqHigh = 6000.0;

    double volume = kDefaultVolume;
    double volumeRamp = 0.0;
    double volumeStepUp = 0.0, volumeStepDown = 0.0;
    static constexpr int kFilterDelayFrames = (FirCrossover::kFirLen - 1) / 2 + (FirCrossover::kEqLen - 1) / 2;
    int rampHoldFrames = kFilterDelayFrames;
    double makeupGainDb = 0.0, makeupGainLinear = 1.0;

    FirCrossover crossoverL;
    FirCrossover crossoverR;
    std::vector<double> lowL, midL, highL, lowR, midR, highR;
    std::vector<double> inputFifoL, inputFifoR, outputFifoL, outputFifoR;

    // Rueckkopplungsspeicher: RACE-Ausgang
    std::vector<double> delayBufferL, delayBufferR;
    int writeIndex = 0;

    // Asymmetrie (Pre-Alignment, links)
    bool asymEnabled = false;
    double asymDelayMs = 0.0, asymDelaySamples = 2.0, asymGainL = 1.0;
    std::vector<double> asymBufferL;
    int asymWriteIndex = 0;

    // EQ (phasenlinear)
    EqBandDef eqBands[3];
    std::vector<double> eqMagnitude;
    fftw_complex* eqDesignSpec;
    double* eqDesignTime;
    double* eqIrTime;
    fftw_complex* eqSpec;
    fftw_complex* flatSpec;
    fftw_plan planEqInverse;
    fftw_plan planEqForward;

    // Taktausgleich
    DriftCompensator drift;
    bool driftEnabled = true;
    bool driftStarted = false;
    std::vector<float> driftFifo;       // interleaved
    std::vector<float> driftOut;        // interleaved
    std::atomic<long> pendingFrames{0};
    std::atomic<int> generation{0};

    // Analyzer
    std::vector<float> spectrumBuffer;
    int spectrumIndex = 0;
    std::vector<float> previousBands;

    // Aufnahme
    LockFreeRingBuffer* recordQueueL;
    LockFreeRingBuffer* recordQueueR;
    std::thread* writerThread = nullptr;
    std::atomic<bool> isRecording{false};
    std::string currentRecordPath;
    int recordSampleRate = 48000;
    void diskWriterLoop();
    void writeWavHeader(std::ofstream& file, uint32_t numFrames, uint32_t sampleRate);
};
#endif /* __cplusplus */

// --- C-WRAPPER FUER DEN SWIFT BRIDGING-HEADER ---
#ifdef __cplusplus
extern "C" {
#else
#include <stdbool.h>
#endif

void* createRACEEngine(float initialDn, float initialAttenuation, float initialCenterP, bool initialFreqLimit, int sampleRate);
void destroyRACEEngine(void* enginePtr);

void wrapper_setSampleRate(void* enginePtr, int sampleRate);
void wrapper_resetStream(void* enginePtr);
void wrapper_setCenterLevel(void* enginePtr, float level);
void wrapper_setParameters(void* enginePtr, float dn, float attenuation, float centerP, bool freqLimit);
void wrapper_setRaceEnabled(void* enginePtr, bool enabled);
void wrapper_setLowBypassEnabled(void* enginePtr, bool enabled);
void wrapper_setVolume(void* enginePtr, float volume);
void wrapper_setMakeupGain(void* enginePtr, float gainDb);

void wrapper_setEqEnabled(void* enginePtr, bool enabled);
void wrapper_setEqBand(void* enginePtr, int band, float freq, float q, float gain);
void wrapper_getEqCurve(void* enginePtr, float* outBuffer, int numPoints);

void wrapper_setAsymmetry(void* enginePtr, bool enabled, float delayMs, float attenuationDb);

void wrapper_processSamples(void* enginePtr, float* leftBuffer, float* rightBuffer, int numFrames);
void wrapper_getSpectrumBands(void* enginePtr, float* outBuffer, int numBands);

void wrapper_pushInput(void* enginePtr, const float* left, const float* right, int numFrames);
int  wrapper_pullOutput(void* enginePtr, float* left, float* right, int numFrames);
void wrapper_outputConsumed(void* enginePtr, int numFrames, int generation);
int  wrapper_getStreamGeneration(void* enginePtr);
void wrapper_setDriftEnabled(void* enginePtr, bool enabled);
double wrapper_getDriftPpm(void* enginePtr);
long wrapper_getBufferedFrames(void* enginePtr);

void wrapper_startRecording(void* enginePtr, const char* filePath, int sampleRate);
void wrapper_stopRecording(void* enginePtr);
void wrapper_enqueueRecordSamples(void* enginePtr, float* leftBuffer, float* rightBuffer, int numFrames);

void wrapper_setFirCrossover(void* enginePtr, float freqLow, float freqHigh);

#ifdef __cplusplus
}
#endif

#endif /* RACEDspEngine_h */
