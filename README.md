# OAPW: Open Ambiophonics Processing Wrapper (macOS)

**OAPW** is a real-time Digital Signal Processing (DSP) application for macOS that performs **Recursive Ambiophonic Crosstalk Elimination (RACE)**. It cancels the interaural crosstalk that occurs when a stereo recording is played over two loudspeakers, which widens the soundstage and sharpens the localization of instruments and voices.

The band split around the RACE stage uses phase-linear FIR filters computed with the Fast Fourier Transform (FFT). Unlike conventional IIR crossovers, these filters add no phase distortion at the crossover frequencies, so the RACE-processed band and the untouched bass and treble recombine cleanly.

OAPW combines a **C++17 DSP engine** (64-bit double precision) with a native **SwiftUI** user interface. The DSP core is the same as in the Raspberry Pi version of OAPW_Player (V21.1).

## 🚀 Key Features

* **True recursive RACE:** The cancellation signal is taken from the processed output and fed back with a delay, as in the original RACE algorithm. The delay is set in microseconds and applied with fractional-sample (Hermite) interpolation, so it stays accurate at 44.1 and 48 kHz. Feedback gain and center parameter are limited to the stable range, and the feedback path is protected against NaN/Inf and denormals.
* **Phase-linear 3-band FIR crossover:** Windowed-sinc filters (32,767 taps, Blackman-Harris window) split the signal into low, mid and high bands. By default RACE is applied to the mid band only, while bass and treble pass unprocessed; the bands sum back to the delayed original signal exactly.
* **Phase-linear 3-band parametric EQ:** Peaking filters designed in the frequency domain and converted to a 32,769-tap linear-phase FIR. Switching the EQ on or off does not change the overall delay.
* **Overlap-Save FFT convolution:** 131,072-point FFT with a block size of 32,768 samples, dimensioned so that crossover and EQ together fit without circular (wrap-around) errors at block boundaries.
* **Clock drift compensation:** BlackHole and your audio interface run on independent clocks that differ by a few ppm. OAPW measures the buffer fill level and continuously resamples by up to ±200 ppm (16-tap Kaiser-windowed sinc interpolator, slow PI control loop). This prevents dropouts and slowly growing delay during long listening sessions. The current correction is printed to the console once per minute.
* **Speaker-safe volume handling:** Playback starts at a low default volume, fades in after (re)start, and all volume changes are ramped (slow up, fast down) to avoid clicks and sudden level jumps.
* **Asymmetric pre-alignment (beta):** Delay and gain pre-alignment of the left channel for off-center listening positions, e.g. the driver's seat of a left-hand-drive car (shown as "GLC Asym" for "Mercedes GLC Asymmetric").
* **WAV export:** 32-bit float WAV recording on a separate writer thread, either RAW (unprocessed input) or processed. A single file is limited to 4 GiB (about 3 hours at 48 kHz stereo).
* **Spectrum analyzer and EQ curve display:** The analyzer shows the processed output signal.

## 🛠 Requirements & Installation

* Mac with Apple Silicon (developed and tested on an M1 MacBook Pro)
* Xcode 16 or newer (the project uses folder-synchronized groups)
* [Homebrew](https://brew.sh)

### 1. Virtual audio cable (BlackHole)
```bash
brew install blackhole-2ch
```

### 2. FFTW3 library (double precision)
```bash
brew install fftw
```
In Xcode, make sure that:
* **Header Search Paths** contain `/opt/homebrew/include`
* **Library Search Paths** contain `/opt/homebrew/lib`
* the target links against **`libfftw3`** (double precision, functions `fftw_…`), e.g. via `-lfftw3` in *Other Linker Flags*. The single-precision library `libfftw3f` is not used.

### 3. Build and run
Open the project in Xcode, select the app target and press **⌘R**. On first launch macOS asks for permission to use audio input (BlackHole is treated as a microphone); this must be allowed.

## 🎧 Audio Routing Setup

OAPW reads from the macOS **default input device** and plays to the macOS **default output device**.

1. In **System Settings → Sound → Input**, select **BlackHole 2ch**.
2. In **System Settings → Sound → Output**, select your DAC or speakers (e.g. *MacBook Pro Speakers*).
3. In **Audio MIDI Setup**, set BlackHole 2ch and your output device to the same sample rate (e.g. 48.0 kHz).
4. In your music player, select **BlackHole 2ch** as output device (e.g. in VLC: *Audio → Audio Device → BlackHole 2ch*). The player must offer its own device selection; players that always follow the system output (such as Apple Music) cannot be used this way.
5. Start OAPW. It captures the BlackHole stream, applies the DSP and plays the result on your speakers.

No Aggregate Device is needed. BlackHole and the output device run on independent clocks; OAPW compensates the difference itself (see *Clock drift compensation*). After start, the console shows the correction once per minute; it settles to a stable value within one to two hours.

**Latency:** The long phase-linear filters and the drift buffer cause a delay of about 1.9 s at 48 kHz. OAPW is intended for music playback, not for video or live monitoring. Controls such as volume act immediately.

**Known limitation:** Switching the output device while OAPW is running (e.g. from speakers to headphones) restarts the audio engine, and plug & play device switching is not yet fully supported. A dedicated edition is planned.

## 🧠 Architecture Overview

* **AudioEngineManager (Swift):** Manages `AVAudioEngine`, the input tap and the player node, and restarts the engine after hardware configuration changes. Input blocks are handed to the engine on the input clock; processed blocks are fetched on the output side, and playback progress is reported back for drift control.
* **RACEDspEngine (C++):** Crossover and EQ (FFTW, 64-bit), recursive RACE, volume ramp, analyzer data and WAV export. All parameter changes are synchronized with the audio processing.
* **DriftCompensator (C++):** Fractional resampler and PI controller for clock drift compensation (shared with the Raspberry Pi version).
* **AmbiophonicsDashboard (SwiftUI):** User interface with spectrum analyzer and EQ curve display.

## 📜 License

The OAPW source code is released under the MIT License (see `LICENSE`).

OAPW links against [FFTW](https://www.fftw.org), which is licensed under the GNU General Public License (GPL v2 or later). If you distribute compiled binaries of OAPW, the combined program is subject to the terms of the GPL.
