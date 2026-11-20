import AVFoundation

class AudioEngineManager {
    static let shared = AudioEngineManager()
    
    private let engine = AVAudioEngine()
    private let playerNode = AVAudioPlayerNode()
    
    var dspEnginePtr: UnsafeMutableRawPointer? = nil
    
    var isLowLatencyMode: Bool = false {
        didSet { updateBufferSize() }
    }
    
    private(set) var isRecording = false
    private(set) var isRecordingProcessed = false
    
    private var powerActivity: NSObjectProtocol?
    private var restartPending = false
    
    // Blockgroesse, in der die Engine an den Player liefert
    private let outputChunkFrames: AVAudioFrameCount = 2048
    private var nextDriftLog = Date().addingTimeInterval(60)
    
    private init() {
        dspEnginePtr = createRACEEngine(68.0, -2.3, 0.5, true, 48000)
        
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleConfigurationChange),
            name: .AVAudioEngineConfigurationChange,
            object: nil
        )
    }
    
    deinit {
        engine.stop()
        if let ptr = dspEnginePtr {
            destroyRACEEngine(ptr)
        }
    }
    
    @objc private func handleConfigurationChange() {
        print("AVAudioEngine: Configuration Change erkannt. Starte Engine neu...")
        
        engine.stop()
        engine.reset()
        
        // Mehrere Meldungen kurz hintereinander loesen nur einen Neustart aus
        DispatchQueue.main.async {
            guard !self.restartPending else { return }
            self.restartPending = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                self.restartPending = false
                self.setupAndStart()
            }
        }
    }
        
    func setupAndStart() {
        if powerActivity == nil {
            powerActivity = ProcessInfo.processInfo.beginActivity(
                options: [.userInitiated, .latencyCritical, .idleSystemSleepDisabled],
                reason: "OAPW Realtime Audio DSP"
            )
        }
        
        engine.stop()
        engine.reset()
        
        let inputNode = engine.inputNode
        let mainMixer = engine.mainMixerNode
        let inputFormat = inputNode.outputFormat(forBus: 0)
        
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            print("Kein gueltiges Eingabegeraet (0 Hz). Engine wird nicht gestartet.")
            return
        }
        
        let hardwareSampleRate = Int32(inputFormat.sampleRate)
        if let ptr = dspEnginePtr {
            wrapper_setSampleRate(ptr, hardwareSampleRate)
            wrapper_resetStream(ptr)   // Filter-/Drift-Zustand leeren, Lautstaerke rampt neu
        }
        
        if !engine.attachedNodes.contains(playerNode) {
            engine.attach(playerNode)
        }
        
        engine.connect(playerNode, to: mainMixer, format: inputFormat)
        
        mainMixer.outputVolume = 1.0
        playerNode.volume = 1.0
        
        installTap(withMode: isLowLatencyMode)
        
        do {
            engine.prepare()
            try engine.start()
            playerNode.play()
        } catch {
            print("Fehler beim Starten der AudioEngine: \(error.localizedDescription)")
        }
    }
    
    private func updateBufferSize() {
        guard engine.isRunning else { return }
        let inputNode = engine.inputNode
        inputNode.removeTap(onBus: 0)
        installTap(withMode: isLowLatencyMode)
    }
    
    private func installTap(withMode lowLatency: Bool) {
        let inputNode = engine.inputNode
        let inputFormat = inputNode.outputFormat(forBus: 0)
        let bufferSize: AVAudioFrameCount = lowLatency ? 256 : 1024
        
        inputNode.removeTap(onBus: 0)
        inputNode.installTap(onBus: 0, bufferSize: bufferSize, format: inputFormat) { [weak self] (buffer, time) in
            self?.processAudioBuffer(buffer: buffer)
        }
    }
    
    func startRecording(to url: URL, asProcessed: Bool) {
        let path = url.path
        let sampleRate = Int32(engine.inputNode.outputFormat(forBus: 0).sampleRate)
        
        path.withCString { cString in
            if let ptr = dspEnginePtr {
                wrapper_startRecording(ptr, cString, sampleRate)
            }
        }
        
        isRecordingProcessed = asProcessed
        isRecording = true
        print("Raw-Aufnahme (Lock-Free) gestartet: \(path) | Modus: \(asProcessed ? "PROCESSED" : "RAW")")
    }
    
    func setCrossover(freqLow: Float, freqHigh: Float) {
        if let ptr = dspEnginePtr {
            wrapper_setFirCrossover(ptr, freqLow, freqHigh)
            print("FIR Crossover aktualisiert: Low = \(freqLow) Hz, High = \(freqHigh) Hz")
        }
    }

    func stopRecording() {
        if let ptr = dspEnginePtr {
            wrapper_stopRecording(ptr)
        }
        isRecording = false
        print("Aufnahme beendet.")
    }
    
    private func processAudioBuffer(buffer: AVAudioPCMBuffer) {
        let frameLength = buffer.frameLength
        guard frameLength > 0,
              let srcData = buffer.floatChannelData,
              let ptr = dspEnginePtr else { return }
        
        let srcChannels = Int(buffer.format.channelCount)
        let srcLeft = srcData[0]
        let srcRight = srcChannels > 1 ? srcData[1] : srcData[0]
        
        // --- EXPORT: UNBEARBEITET (RAW) ---
        if isRecording && !isRecordingProcessed {
            wrapper_enqueueRecordSamples(ptr, srcLeft, srcRight, Int32(frameLength))
        }
        
        // --- Eingangstakt: nur abgeben, der DSP laeuft beim Abholen ---
        wrapper_pushInput(ptr, srcLeft, srcRight, Int32(frameLength))
        
        if !playerNode.isPlaying {
            playerNode.play()
        }
        
        // --- Alle fertigen Bloecke an den Player geben ---
        while let out = AVAudioPCMBuffer(pcmFormat: buffer.format, frameCapacity: outputChunkFrames),
              let dst = out.floatChannelData {
            let left = dst[0]
            var mix = [Float](repeating: 0, count: Int(outputChunkFrames))
            
            let got: Int32 = mix.withUnsafeMutableBufferPointer { monoRight in
                let right = srcChannels > 1 ? dst[1] : monoRight.baseAddress!
                return wrapper_pullOutput(ptr, left, right, Int32(outputChunkFrames))
            }
            if got == 0 { break }
            
            out.frameLength = AVAudioFrameCount(got)
            for ch in 2..<max(2, srcChannels) {
                memset(dst[ch], 0, Int(got) * MemoryLayout<Float>.size)
            }
            
            // --- EXPORT: BEARBEITET (PROCESSED) ---
            if isRecording && isRecordingProcessed {
                let right = srcChannels > 1 ? dst[1] : dst[0]
                wrapper_enqueueRecordSamples(ptr, left, right, got)
            }
            
            let generation = wrapper_getStreamGeneration(ptr)
            playerNode.scheduleBuffer(out, completionCallbackType: .dataConsumed) { _ in
                wrapper_outputConsumed(ptr, got, generation)
            }
        }
        
        // Taktausgleich beobachten: Korrektur sollte langsam auf einen festen Wert laufen
        if Date() >= nextDriftLog {
            nextDriftLog = Date().addingTimeInterval(60)
            print(String(format: "Drift: %.2f ppm, Puffer %ld Frames",
                         wrapper_getDriftPpm(ptr), wrapper_getBufferedFrames(ptr)))
        }
    }
}
