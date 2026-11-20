import SwiftUI
import Combine
import AppKit
import UniformTypeIdentifiers

struct AmbiophonicsDashboard: View {
    // Globale Status
    @State private var isRunning = true
    @State private var isAmbiophonicsActive = true
    @State private var processingMode = 0 // 0 = Music, 1 = Movie
    @State private var isRecording = false
    
    // Status für den 3-Wege Export-Modus (0 = RAW, 1 = Normal, 2 = GLC)
    @State private var recordMode = 0
    
    // RACE Parameters
    @State private var attenuation: Float = -2.3
    @State private var speakerDelay: Float = 68.0
    @State private var centerLevel: Float = 0.0
    @State private var volumePercent: Float = 100.0
    @State private var crossoverLow: Float = 250.0
    @State private var crossoverHigh: Float = 6000.0

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            
            // --- HEADER ---
            VStack(alignment: .leading, spacing: 5) {
                Text("Recursive Ambiophonic Crosstalk Elimination")
                    .font(.title3)
                    .foregroundColor(.secondary)
                
                Text(isRunning ? "DSP Engine Running" : "DSP Engine Stopped")
                    .font(.subheadline)
                    .bold()
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .background(isRunning ? Color.green.opacity(0.2) : Color.red.opacity(0.2))
                    .foregroundColor(isRunning ? .green : .red)
                    .cornerRadius(8)
            }
            .padding(.bottom, 5)
            
            // --- HAUPTBEREICH (RACE & Routing/Geometrie nebeneinander) ---
            HStack(alignment: .top, spacing: 20) {
                
                // 1. RACE Parameters Panel (Links)
                GroupBox(label:
                    HStack {
                        Label("RACE Parameters", systemImage: "waveform.path.ecg")
                        Spacer()
                        Button(action: resetRACEParameters) {
                            Label("Reset", systemImage: "arrow.counterclockwise")
                        }
                        .buttonStyle(.bordered)
                    }
                )
                {
                    VStack(spacing: 20) {
                        parameterRow(name: "Attenuation", value: $attenuation, range: -10...0, unit: "dB", fractionDigits: 1)
                        parameterRow(name: "Speaker Delay", value: $speakerDelay, range: 0...100, unit: "µs", fractionDigits: 0)
                        parameterRow(name: "Center Level", value: $centerLevel, range: 0...1, unit: "", fractionDigits: 2)
                        parameterRow(name: "Volume", value: $volumePercent, range: 0...200, unit: "%", fractionDigits: 0)
                        
                        Divider()
                        
                        // --- Trennfrequenz Unten (Highpass) ---
                        VStack(alignment: .leading, spacing: 5) {
                            Text("RACE Highpass (Bässe) [Hz]").font(.subheadline)
                            HStack {
                                Slider(value: $crossoverLow, in: 20...1500)
                                TextField("", value: $crossoverLow, format: .number.precision(.fractionLength(0)))
                                    .textFieldStyle(RoundedBorderTextFieldStyle())
                                    .frame(width: 55)
                                    .multilineTextAlignment(.trailing)
                                    .font(.caption)
                            }
                        }
                        .onChange(of: crossoverLow) { oldValue, newValue in
                            if newValue >= crossoverHigh { crossoverHigh = newValue + 100 }
                            updateCrossover()
                        }
                        
                        // --- Trennfrequenz Oben (Lowpass) ---
                        VStack(alignment: .leading, spacing: 5) {
                            Text("RACE Lowpass (Höhen) [Hz]").font(.subheadline)
                            HStack {
                                Slider(value: $crossoverHigh, in: 1000...20000)
                                TextField("", value: $crossoverHigh, format: .number.precision(.fractionLength(0)))
                                    .textFieldStyle(RoundedBorderTextFieldStyle())
                                    .frame(width: 55)
                                    .multilineTextAlignment(.trailing)
                                    .font(.caption)
                            }
                        }
                        .onChange(of: crossoverHigh) { oldValue, newValue in
                            if newValue <= crossoverLow { crossoverLow = newValue - 100 }
                            updateCrossover()
                        }
                    }
                    .padding(.top, 15)
                }
                
                // Rechte Spalte: Routing und Geometrie untereinander
                VStack(spacing: 20) {
                    
                    // 2. Audio Routing Panel (Oben Rechts)
                    GroupBox(label: Label("Audio Routing & Export", systemImage: "arrow.left.and.right.square")) {
                        VStack(alignment: .leading, spacing: 15) {
                            HStack {
                                Text("Input").foregroundColor(.secondary)
                                Spacer()
                                Text("BlackHole 2ch")
                            }
                            Divider()
                            HStack {
                                Text("Output").foregroundColor(.secondary)
                                Spacer()
                                Text("MacBook Pro")
                            }
                            
                            Spacer().frame(height: 5)
                            
                            // --- 3-Wege Selektor für den WAV-Export ---
                            VStack(alignment: .leading, spacing: 5) {
                                Text("WAV Export Mode").font(.caption).foregroundColor(.secondary)
                                Picker("", selection: $recordMode) {
                                    Text("1. RAW (Bypass)").tag(0)
                                    Text("2. Std. RACE").tag(1)
                                    Text("3. GLC Asym.").tag(2)
                                }
                                .pickerStyle(SegmentedPickerStyle())
                                .labelsHidden()
                                .disabled(isRecording)
                            }
                            
                            // --- Dynamischer Aufnahme Button ---
                            Button(action: toggleRecording) {
                                HStack {
                                    Circle()
                                        .fill(isRecording ? Color.red : Color.gray)
                                        .frame(width: 10, height: 10)
                                    Text(isRecording ? "Stop Recording" : "Start Export...")
                                }
                                .frame(maxWidth: .infinity)
                                .padding(4)
                            }
                            .buttonStyle(.bordered)
                            .tint(isRecording ? .red : .primary)
                            
                            Spacer().frame(height: 5)
                            
                            Text("Processing Mode").font(.subheadline).foregroundColor(.secondary)
                            Picker("", selection: $processingMode) {
                                Text("Music (HQ)").tag(0)
                                Text("Movie (Low Latency)").tag(1)
                            }
                            .pickerStyle(SegmentedPickerStyle())
                            
                            Spacer()
                            
                            Button(action: {
                                isAmbiophonicsActive.toggle()
                                if let dspPtr = AudioEngineManager.shared.dspEnginePtr {
                                    wrapper_setRaceEnabled(dspPtr, isAmbiophonicsActive)
                                }
                            }) {
                                Label(isAmbiophonicsActive ? "Ambiophonics Active" : "Ambiophonics Bypassed", systemImage: isAmbiophonicsActive ? "speaker.wave.2.fill" : "speaker.slash.fill")
                                    .frame(maxWidth: .infinity)
                                    .padding(6)
                            }
                            .buttonStyle(.borderedProminent)
                            .tint(isAmbiophonicsActive ? .blue : .gray)
                            .controlSize(.large)
                        }
                        .padding(.top, 15)
                    }
                    
                    // 3. Geometrie Rechner (Unten Rechts)
                    GeometryCalculatorView(
                        targetDelay: $speakerDelay,
                        targetAttenuation: $attenuation,
                        onApply: updateRACEParameters
                    )
                }
            }
            
            // --- FFT SPECTRUM ANALYZER ---
            SpectrumAnalyzerView()
                        
            // --- EQ CONTROL PANEL (Ganz unten) ---
            EQControlPanel()
            
        }
        .padding(25)
        .onAppear {
            AudioEngineManager.shared.setupAndStart()
            updateRACEParameters()
        }
    }
    
    // --- Dynamischer Dialog-Aufruf für den verlustfreien WAV-Export ---
    private func toggleRecording() {
        if isRecording {
            AudioEngineManager.shared.stopRecording()
            isRecording = false
        } else {
            let panel = NSSavePanel()
            panel.allowedContentTypes = [UTType.wav]
            panel.canCreateDirectories = true
            
            if recordMode == 0 {
                panel.nameFieldStringValue = "stream_raw_unprocessed.wav"
            } else if recordMode == 1 {
                panel.nameFieldStringValue = "stream_processed_std_race.wav"
            } else {
                panel.nameFieldStringValue = "stream_processed_glc_asym.wav"
            }
            
            panel.title = "Speicherort für 32-bit Float Audio wählen"
            
            panel.begin { response in
                if response == .OK, let url = panel.url {
                    if let dspPtr = AudioEngineManager.shared.dspEnginePtr {
                        if recordMode == 2 {
                            wrapper_setAsymmetry(dspPtr, true, 1.37, -3.9)
                        } else {
                            wrapper_setAsymmetry(dspPtr, false, 0.0, 0.0)
                        }
                    }
                    
                    let asProcessed = (recordMode != 0)
                    AudioEngineManager.shared.startRecording(to: url, asProcessed: asProcessed)
                    isRecording = true
                }
            }
        }
    }
    
    private func updateRACEParameters() {
        if let dspPtr = AudioEngineManager.shared.dspEnginePtr {
            wrapper_setParameters(dspPtr, speakerDelay, attenuation, 0.5, true)
            wrapper_setCenterLevel(dspPtr, centerLevel)
            wrapper_setVolume(dspPtr, volumePercent / 100.0)
            wrapper_setFirCrossover(dspPtr, crossoverLow, crossoverHigh)
        }
    }
    
    private func updateCrossover() {
        if let dspPtr = AudioEngineManager.shared.dspEnginePtr {
            wrapper_setFirCrossover(dspPtr, crossoverLow, crossoverHigh)
        }
    }
    
    private func resetRACEParameters() {
        attenuation = -2.3
        speakerDelay = 68.0
        centerLevel = 0.0
        volumePercent = 100.0
        crossoverLow = 250.0
        crossoverHigh = 6000.0
        updateRACEParameters()
    }
    
    @ViewBuilder
    private func parameterRow(name: String, value: Binding<Float>, range: ClosedRange<Float>, unit: String, fractionDigits: Int = 1) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(unit.isEmpty ? name : "\(name) [\(unit)]").font(.subheadline)
            
            HStack {
                Slider(value: value, in: range)
                
                TextField("", value: value, format: .number.precision(.fractionLength(fractionDigits)))
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .frame(width: 55)
                    .multilineTextAlignment(.trailing)
                    .font(.caption)
            }
        }
        .onChange(of: value.wrappedValue) {
            updateRACEParameters()
        }
    }
}

// --------------------------------------------------------
// --- DAS 3-BAND EQ MODUL ---
// --------------------------------------------------------

struct EQCurveGraphView: View {
    @Binding var redrawTrigger: Int
    @State private var curveData: [Float] = Array(repeating: 0.0, count: 200)
    
    var body: some View {
        GeometryReader { geo in
            let midY = geo.size.height / 2
            
            Path { path in
                path.move(to: CGPoint(x: 0, y: midY))
                path.addLine(to: CGPoint(x: geo.size.width, y: midY))
            }
            .stroke(Color.gray.opacity(0.4), style: StrokeStyle(lineWidth: 1, dash: [5]))
            
            Path { path in
                for i in 0..<curveData.count {
                    let x = geo.size.width * CGFloat(i) / CGFloat(curveData.count - 1)
                    let normalizedY = 1.0 - CGFloat(curveData[i] + 15.0) / 30.0
                    let y = geo.size.height * min(max(normalizedY, 0.0), 1.0)
                    
                    if i == 0 { path.move(to: CGPoint(x: x, y: y)) }
                    else { path.addLine(to: CGPoint(x: x, y: y)) }
                }
            }
            .stroke(Color.blue, style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
        }
        .frame(height: 100)
        .padding(10)
        .background(Color(white: 0.1))
        .cornerRadius(6)
        .onChange(of: redrawTrigger) {
            fetchCurve()
        }
        .onAppear {
            fetchCurve()
        }
    }
    
    private func fetchCurve() {
        guard let dspPtr = AudioEngineManager.shared.dspEnginePtr else { return }
        var rawData = [Float](repeating: 0.0, count: 200)
        wrapper_getEqCurve(dspPtr, &rawData, Int32(rawData.count))
        self.curveData = rawData
    }
}

struct EQControlPanel: View {
    @AppStorage("eqEnabled") private var isEQEnabled: Bool = false
    
    @AppStorage("eqLowFreq") private var lowFreq: Double = 100.0
    @AppStorage("eqLowQ") private var lowQ: Double = 0.707
    @AppStorage("eqLowGain") private var lowGain: Double = 0.0
    
    @AppStorage("eqMidFreq") private var midFreq: Double = 1000.0
    @AppStorage("eqMidQ") private var midQ: Double = 0.707
    @AppStorage("eqMidGain") private var midGain: Double = 0.0
    
    @AppStorage("eqHighFreq") private var highFreq: Double = 5000.0
    @AppStorage("eqHighQ") private var highQ: Double = 0.707
    @AppStorage("eqHighGain") private var highGain: Double = 0.0
    
    @State private var curveRedrawTrigger: Int = 0
    
    var body: some View {
        GroupBox(label:
            HStack {
                Label("3-Band Parametric EQ (FIR Phase-Linear)", systemImage: "slider.horizontal.3")
                    .font(.headline)
                Spacer()
                Button(action: resetEQ) {
                    Label("Reset", systemImage: "arrow.counterclockwise")
                }
                .buttonStyle(.bordered)
                .padding(.trailing, 10)
                
                Toggle("Aktiv", isOn: $isEQEnabled)
                    .toggleStyle(SwitchToggleStyle(tint: .blue))
                    .onChange(of: isEQEnabled) { oldValue, newValue in
                        if let dspPtr = AudioEngineManager.shared.dspEnginePtr {
                            wrapper_setEqEnabled(dspPtr, newValue)
                        }
                    }
            }
        ) {
            VStack(spacing: 15) {
                EQCurveGraphView(redrawTrigger: $curveRedrawTrigger)
                    .opacity(isEQEnabled ? 1.0 : 0.4)
                
                HStack(spacing: 30) {
                    eqBandView(name: "Low", freq: $lowFreq, freqRange: 20...250, q: $lowQ, gain: $lowGain, bandIndex: 0)
                    Divider()
                    eqBandView(name: "Mid", freq: $midFreq, freqRange: 250...4000, q: $midQ, gain: $midGain, bandIndex: 1)
                    Divider()
                    eqBandView(name: "High", freq: $highFreq, freqRange: 4000...16000, q: $highQ, gain: $highGain, bandIndex: 2)
                }
                .padding(.top, 5)
            }
            .padding(.bottom, 5)
            .disabled(!isEQEnabled)
        }
        .onAppear {
            sendToDSP(band: 0, f: lowFreq, q: lowQ, g: lowGain)
            sendToDSP(band: 1, f: midFreq, q: midQ, g: midGain)
            sendToDSP(band: 2, f: highFreq, q: highQ, g: highGain)
            
            if let dspPtr = AudioEngineManager.shared.dspEnginePtr {
                wrapper_setEqEnabled(dspPtr, isEQEnabled)
            }
        }
    }
    
    private func resetEQ() {
        isEQEnabled = false
        lowGain = 0.0
        midGain = 0.0
        highGain = 0.0
        
        sendToDSP(band: 0, f: lowFreq, q: lowQ, g: 0.0)
        sendToDSP(band: 1, f: midFreq, q: midQ, g: 0.0)
        sendToDSP(band: 2, f: highFreq, q: highQ, g: 0.0)
        
        if let dspPtr = AudioEngineManager.shared.dspEnginePtr {
            wrapper_setEqEnabled(dspPtr, false)
        }
    }
    
    @ViewBuilder
    private func eqBandView(name: String, freq: Binding<Double>, freqRange: ClosedRange<Double>, q: Binding<Double>, gain: Binding<Double>, bandIndex: Int32) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("\(name) Band").font(.subheadline).bold()
            
            HStack {
                Text("Freq [Hz]:").frame(width: 65, alignment: .leading).font(.caption)
                Slider(value: freq, in: freqRange)
                TextField("", value: freq, format: .number.precision(.fractionLength(0)))
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .frame(width: 55)
                    .multilineTextAlignment(.trailing)
                    .font(.caption)
            }
            HStack {
                Text("Q:").frame(width: 65, alignment: .leading).font(.caption)
                Slider(value: q, in: 0.1...10.0)
                TextField("", value: q, format: .number.precision(.fractionLength(2)))
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .frame(width: 55)
                    .multilineTextAlignment(.trailing)
                    .font(.caption)
            }
            HStack {
                Text("Gain [dB]:").frame(width: 65, alignment: .leading).font(.caption)
                Slider(value: gain, in: -12.0...12.0)
                TextField("", value: gain, format: .number.precision(.fractionLength(1)))
                    .textFieldStyle(RoundedBorderTextFieldStyle())
                    .frame(width: 55)
                    .multilineTextAlignment(.trailing)
                    .font(.caption)
            }
        }
        .onChange(of: freq.wrappedValue) { sendToDSP(band: bandIndex, f: freq.wrappedValue, q: q.wrappedValue, g: gain.wrappedValue) }
        .onChange(of: q.wrappedValue) { sendToDSP(band: bandIndex, f: freq.wrappedValue, q: q.wrappedValue, g: gain.wrappedValue) }
        .onChange(of: gain.wrappedValue) { sendToDSP(band: bandIndex, f: freq.wrappedValue, q: q.wrappedValue, g: gain.wrappedValue) }
    }
    
    private func sendToDSP(band: Int32, f: Double, q: Double, g: Double) {
        guard let dspPtr = AudioEngineManager.shared.dspEnginePtr else { return }
        wrapper_setEqBand(dspPtr, band, Float(f), Float(q), Float(g))
        curveRedrawTrigger += 1
    }
}

// --------------------------------------------------------
// --- SPECTRUM ANALYZER MODUL (Gradient Mask Version) ---
// --------------------------------------------------------

struct SpectrumAnalyzerView: View {
    @State private var spectrumData: [Float] = Array(repeating: 0.1, count: 32)
    let updateTimer = Timer.publish(every: 1.0 / 30.0, on: .main, in: .common).autoconnect()
    
    let barGradient = LinearGradient(
        stops: [
            .init(color: Color(red: 0.0, green: 0.2, blue: 0.7), location: 0.0),
            .init(color: Color(red: 0.0, green: 0.3, blue: 0.9), location: 0.75),
            .init(color: .red, location: 0.90),
            .init(color: .purple, location: 1.0)
        ],
        startPoint: .bottom,
        endPoint: .top
    )
    
    var body: some View {
        GroupBox(label: Label("Realtime FFT Spectrum - logarithmic scale from 20Hz to 20kHz", systemImage: "waveform")) {
            VStack(spacing: 8) {
                
                HStack(alignment: .bottom, spacing: 3) {
                    ForEach(0..<spectrumData.count, id: \.self) { i in
                        let level = CGFloat(spectrumData[i])
                        let barHeight = max(level * 120.0, 4.0)
                        
                        Rectangle()
                            .fill(barGradient)
                            .frame(height: 120)
                            .mask(
                                VStack {
                                    Spacer(minLength: 0)
                                    RoundedRectangle(cornerRadius: 2)
                                        .frame(height: barHeight)
                                }
                            )
                            .animation(.linear(duration: 0.05), value: spectrumData[i])
                    }
                }
                .frame(maxWidth: .infinity)
                .frame(height: 120)
                
                GeometryReader { geo in
                    let w = geo.size.width
                    
                    Text("20 Hz").position(x: 15, y: 10)
                    Text("100 Hz").position(x: w * 0.233, y: 10)
                    Text("1 kHz").position(x: w * 0.566, y: 10)
                    Text("10 kHz").position(x: w * 0.900, y: 10)
                    Text("20 kHz").position(x: w - 20, y: 10)
                }
                .frame(height: 20)
                .font(.system(size: 10, weight: .medium))
                .foregroundColor(Color.gray)
                
            }
            .padding(.horizontal, 15)
            .padding(.vertical, 15)
            .background(Color(white: 0.08))
            .cornerRadius(6)
            .padding(.top, 10)
        }
        .onReceive(updateTimer) { _ in
            fetchFFTData()
        }
    }
    
    private func fetchFFTData() {
        guard let dspPtr = AudioEngineManager.shared.dspEnginePtr else { return }
        var rawFFT = [Float](repeating: 0.0, count: 32)
        wrapper_getSpectrumBands(dspPtr, &rawFFT, 32)
        self.spectrumData = rawFFT
    }
}

// --------------------------------------------------------
// --- RACE GEOMETRIE RECHNER ---
// --------------------------------------------------------

struct GeometryCalculatorView: View {
    @Binding var targetDelay: Float
    @Binding var targetAttenuation: Float
    var onApply: () -> Void

    @State private var listenerDistance: Double = 60.0
    @State private var speakerSeparation: Double = 30.0
    @State private var headWidth: Double = 17.5

    var body: some View {
        GroupBox(label: Label("Geometry Calculator", systemImage: "ruler")) {
            VStack(spacing: 15) {
                HStack {
                    Text("Distance Head Baseline [cm]").font(.subheadline)
                    Spacer()
                    Slider(value: $listenerDistance, in: 20...400)
                    TextField("", value: $listenerDistance, format: .number.precision(.fractionLength(0)))
                        .textFieldStyle(RoundedBorderTextFieldStyle())
                        .frame(width: 55)
                        .multilineTextAlignment(.trailing)
                        .font(.caption)
                }
                
                HStack {
                    Text("Speaker Separation [cm]").font(.subheadline)
                    Spacer()
                    Slider(value: $speakerSeparation, in: 10...300)
                    TextField("", value: $speakerSeparation, format: .number.precision(.fractionLength(0)))
                        .textFieldStyle(RoundedBorderTextFieldStyle())
                        .frame(width: 55)
                        .multilineTextAlignment(.trailing)
                        .font(.caption)
                }

                Divider()

                Button(action: calculateAndApply) {
                    Label("Calculate & Apply", systemImage: "arrow.up.right.circle.fill")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .tint(.blue)
            }
            .padding(.top, 10)
        }
    }

    private func calculateAndApply() {
        let c = 34300.0 // Schallgeschwindigkeit in cm/s
        let hw = headWidth / 2.0
        let sw = speakerSeparation / 2.0
        let d = listenerDistance

        let dDirect = sqrt(pow(sw - hw, 2) + pow(d, 2))
        let dCross = sqrt(pow(sw + hw, 2) + pow(d, 2))

        let deltaDistance = dCross - dDirect
        let delayMicroseconds = Float((deltaDistance / c) * 1_000_000)

        let attenuationLinear = dDirect / dCross
        let attenuationDB = Float(20.0 * log10(attenuationLinear))

        targetDelay = min(max(delayMicroseconds, 0.0), 100.0)
        targetAttenuation = max(attenuationDB, -10.0)
        
        onApply()
    }
}
