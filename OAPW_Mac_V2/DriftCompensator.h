#ifndef DRIFT_COMPENSATOR_H
#define DRIFT_COMPENSATOR_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Software-Taktausgleich (Rate Matching) zwischen Loopback-Takt (Capture) und DAC-Quarz.
//
// Problem: Beide Takte weichen um wenige ppm voneinander ab. Ohne Ausgleich wandert der
// Fuellstand des Ringpuffers langsam nach oben oder unten, bis er ueberlaeuft (Capture
// verwirft Samples) oder leerlaeuft (Underrun). Beides ist hoerbar.
//
// Loesung: Der Playback-Thread verbraucht pro Periode nicht genau N, sondern N * r
// Eingangs-Frames, r = 1 + c * 1e-6, und rechnet sie mit einem Bruchteil-Verzoegerungs-
// Interpolator (Kaiser-gefensterter Sinc, 16 Stuetzstellen) auf N Ausgangs-Frames um.
// Die Korrektur c (in ppm) regelt ein sehr langsamer PI-Regler aus dem gemittelten
// Fuellstand. Bei einer Abweichung von 2 ppm entspricht das einer Tonhoehen-Aenderung
// von 0,0002 Promille, weit unter jeder Hoerschwelle.
//
// Eigenschaften:
//   - Bei c = 0 und Phase 0 ist die Ausgabe bitgenau gleich der Eingabe (Zeile 0 der
//     Gewichtstabelle ist ein exaktes Delta).
//   - Zusatzlatenz: 8 Frames (ca. 0,04 ms bei 192 kHz).
//   - Nur im Playback-Thread benutzen (nicht thread-sicher).
class DriftCompensator {
public:
    static constexpr int    kTaps    = 16;   // Stuetzstellen des Interpolators
    static constexpr int    kBefore  = 7;    // Stuetzstellen vor der Position
    static constexpr int    kAfter   = 8;    // Stuetzstellen nach der Position
    static constexpr int    kPhases  = 1024;  // Bruchteil-Raster der Gewichtstabelle

    // Regler-Parameter (siehe Beschreibung in control())
    static constexpr double kMaxPpm      = 200.0;   // Begrenzung der Korrektur
    static constexpr double kAvgTauSec   = 30.0;    // Glaettung des Fuellstands
    static constexpr double kLoopOmega   = 1.0 / 4000.0; // Eigenfrequenz der Regelschleife [1/s]

    DriftCompensator(unsigned channels, double sampleRate)
        : channels_(channels), sampleRate_(sampleRate) {
        buildTable();
        reset();
    }

    // Verwirft die Signalhistorie (Stille am Anfang). Die gelernte Korrektur bleibt erhalten.
    void reset() {
        buf_.assign(static_cast<size_t>(kBefore) * channels_, 0.0f);
        pos_ = static_cast<double>(kBefore);
        avgValid_ = false;
    }

    // Wie viele Eingangs-Frames werden gebraucht, um outFrames auszugeben?
    size_t framesNeeded(size_t outFrames) const {
        if (outFrames == 0) return 0;
        const double lastPos = pos_ + static_cast<double>(outFrames - 1) * ratio_;
        const long n0 = static_cast<long>(std::floor(lastPos));
        const long have = static_cast<long>(buf_.size() / channels_);
        const long need = n0 + kAfter + 1 - have;
        return need > 0 ? static_cast<size_t>(need) : 0;
    }

    // in: genau framesNeeded(outFrames) Frames (interleaved). out: outFrames Frames.
    void process(const float* in, size_t inFrames, float* out, size_t outFrames) {
        buf_.insert(buf_.end(), in, in + inFrames * channels_);

        const double pos0 = pos_;
        for (size_t k = 0; k < outFrames; ++k) {
            const double p = pos0 + static_cast<double>(k) * ratio_;
            const long n0 = static_cast<long>(std::floor(p));
            const double mu = p - static_cast<double>(n0);

            const double x = mu * kPhases;
            int ip = static_cast<int>(x);
            if (ip >= kPhases) ip = kPhases - 1;
            const double f = x - ip;
            const double* a = &table_[static_cast<size_t>(ip) * kTaps];
            const double* b = a + kTaps;

            double w[kTaps];
            for (int j = 0; j < kTaps; ++j) w[j] = a[j] + f * (b[j] - a[j]);

            const float* src = &buf_[static_cast<size_t>(n0 - kBefore) * channels_];
            for (unsigned c = 0; c < channels_; ++c) {
                double acc = 0.0;
                for (int j = 0; j < kTaps; ++j) acc += w[j] * static_cast<double>(src[static_cast<size_t>(j) * channels_ + c]);
                out[k * channels_ + c] = static_cast<float>(acc);
            }
        }

        pos_ = pos0 + static_cast<double>(outFrames) * ratio_;
        const long drop = static_cast<long>(std::floor(pos_)) - kBefore;
        if (drop > 0) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<size_t>(drop) * channels_);
            pos_ -= static_cast<double>(drop);
        }
    }

    // Regler: einmal pro Periode mit dem aktuellen Fuellstand (in Frames) aufrufen.
    //
    // Modell: Der Fuellstand e (in ms) aendert sich um (d - c) * 1e-3 ms pro Sekunde,
    // d = Takt-Drift, c = Korrektur (beide in ppm). Der PI-Regler  c = Kp*e + Ki*Integral(e)
    // ergibt die Schleife  s^2 + 1e-3*Kp*s + 1e-3*Ki = 0. Gewaehlt ist kritische Daempfung
    // bei  omega = 1/900 s:  Kp = 2*omega/1e-3 (ppm/ms),  Ki = omega^2/1e-3.
    // Der Fuellstand wird vorher mit 30 s Zeitkonstante geglaettet, weil er in Spruengen
    // von einer Periode und durch DSP-Bloecke "saegt".
    void control(double fillFrames, double targetFrames, double dtSec) {
        if (!avgValid_) {
            avgFrames_ = fillFrames;
            avgValid_ = true;
        } else {
            const double alpha = 1.0 - std::exp(-dtSec / kAvgTauSec);
            avgFrames_ += alpha * (fillFrames - avgFrames_);
        }
        const double eMs = (avgFrames_ - targetFrames) * 1000.0 / sampleRate_;

        const double kp = 2.0 * kLoopOmega / 1e-3;
        const double ki = kLoopOmega * kLoopOmega / 1e-3;

        integ_ += ki * eMs * dtSec;
        integ_ = std::clamp(integ_, -kMaxPpm, kMaxPpm);   // Anti-Windup
        ppm_ = std::clamp(kp * eMs + integ_, -kMaxPpm, kMaxPpm);
        ratio_ = 1.0 + ppm_ * 1e-6;
    }

    double ppm() const { return ppm_; }
    double averagedFillFrames() const { return avgFrames_; }
    bool atLimit() const { return std::fabs(ppm_) >= kMaxPpm * 0.9; }

    // Nur fuer Tests: Verhaeltnis direkt setzen (ersetzt die Regelung bis zum naechsten control()).
    void setPpmForTest(double ppm) { ppm_ = ppm; ratio_ = 1.0 + ppm * 1e-6; }

private:
    unsigned channels_;
    double sampleRate_;
    std::vector<float> buf_;     // Historie + neue Eingangs-Frames (interleaved)
    double pos_ = 0.0;           // Position des naechsten Ausgangs-Frames in buf_ (in Frames)
    double ratio_ = 1.0;
    double ppm_ = 0.0;
    double integ_ = 0.0;
    double avgFrames_ = 0.0;
    bool avgValid_ = false;
    std::vector<double> table_;  // (kPhases + 1) Zeilen x kTaps Gewichte

    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 60; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-18 * sum) break;
        }
        return sum;
    }

    void buildTable() {
        const double beta = 14.0;   // Kaiser-Parameter (gemessen: Fehler < -135 dB bis 20 kHz)
        const double pi = 3.14159265358979323846;
        const double i0b = besselI0(beta);
        table_.assign(static_cast<size_t>(kPhases + 1) * kTaps, 0.0);
        for (int p = 0; p <= kPhases; ++p) {
            const double mu = static_cast<double>(p) / kPhases;
            double sum = 0.0;
            double* row = &table_[static_cast<size_t>(p) * kTaps];
            for (int j = 0; j < kTaps; ++j) {
                const double t = static_cast<double>(j - kBefore) - mu;   // Abstand Stuetzstelle - Position
                double s = (std::fabs(t) < 1e-12) ? 1.0 : std::sin(pi * t) / (pi * t);
                const double r = t / static_cast<double>(kAfter);         // -1 ... 1 ueber das Fenster
                const double arg = 1.0 - r * r;
                const double win = (arg > 0.0) ? besselI0(beta * std::sqrt(arg)) / i0b : 0.0;
                row[j] = s * win;
                sum += row[j];
            }
            if (p == 0) {
                // Exaktes Delta: bitgenauer Durchlauf bei Phase 0
                for (int j = 0; j < kTaps; ++j) row[j] = (j == kBefore) ? 1.0 : 0.0;
            } else {
                for (int j = 0; j < kTaps; ++j) row[j] /= sum;           // Gleichanteil-Verstaerkung 1
            }
        }
    }
};

#endif // DRIFT_COMPENSATOR_H
