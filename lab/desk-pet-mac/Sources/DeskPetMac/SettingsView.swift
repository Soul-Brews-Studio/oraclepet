import PetCore
import SwiftUI

/// OraclePet Settings (menu "Settings…", ⌘, or deskpet://settings): what applies to every pet.
/// Secrets go to the macOS Keychain, never to UserDefaults or a file.
struct SettingsView: View {
    let pets: [PetLink]
    @AppStorage("voiceLanguage") private var voiceLanguage = "th"
    @AppStorage("voiceEngine") private var voiceEngine = "macos"
    @AppStorage("paxaVoice") private var paxaVoice = "khanomkrok"
    @AppStorage("edgeVoice") private var edgeVoice = "th-TH-NiwatNeural"
    @AppStorage("voiceAnnounce") private var announce = true
    @State private var paxaKey = ""
    @State private var wifiSSID = PetWiFi.ssid ?? ""
    @State private var wifiPass = ""
    @State private var hasKey = PaxaVoice.hasKey

    var body: some View {
        Form {
            Section("Voice") {
                Picker("Engine", selection: $voiceEngine) {
                    Text("macOS (offline)").tag("macos")
                    Text("Paxa TTS Flash v2 (Thai + English)").tag("paxa")
                    Text("Edge neural (free, online)").tag("edge")
                }
                Picker("Language", selection: $voiceLanguage) {
                    Text("ไทย").tag("th")
                    Text("English").tag("en")
                }
                .pickerStyle(.segmented)
                if voiceEngine == "edge" {
                    Picker("Edge voice", selection: $edgeVoice) {
                        ForEach(EdgeVoice.voices, id: \.self) { Text($0).tag($0) }
                    }
                }
                Toggle("Speak when an agent needs you", isOn: $announce)
                Text(voiceEngine == "paxa"
                     ? "Paxa reads Thai and English as written. If a request fails, the macOS voice speaks."
                     : "macOS: English is translated to Thai on this Mac first, names are written in Thai.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("WiFi for voice") {
                TextField("Network (SSID)", text: $wifiSSID)
                SecureField(PetWiFi.ssid != nil && wifiPass.isEmpty ? "Password saved in the Keychain" : "Password", text: $wifiPass)
                HStack {
                    Button("Save") { PetWiFi.save(ssid: wifiSSID, password: wifiPass); wifiPass = "" }
                        .disabled(wifiSSID.isEmpty)
                    Button("Send to the pets") {
                        if !wifiPass.isEmpty { PetWiFi.save(ssid: wifiSSID, password: wifiPass); wifiPass = "" }
                        for p in pets { PetWiFi.send(to: p) }
                    }
                    .disabled(wifiSSID.isEmpty)
                }
                Text("Sent over Bluetooth with Improv; each pet keeps it and finds the voice server on this Mac by itself.")
                    .font(.caption).foregroundStyle(.secondary)
                ForEach(pets.indices, id: \.self) { i in PetVoiceRow(link: pets[i]) }
            }
            Section("Paxa Labs") {
                Picker("Voice", selection: $paxaVoice) {
                    ForEach(PaxaVoice.voices, id: \.self) { Text($0).tag($0) }
                }
                HStack {
                    SecureField(hasKey ? "API key saved in the Keychain" : "API key from paxalabs.com", text: $paxaKey)
                    Button(hasKey && paxaKey.isEmpty ? "Remove" : "Save") {
                        PaxaVoice.setKey(paxaKey)
                        paxaKey = ""
                        hasKey = PaxaVoice.hasKey
                    }
                }
                Label(hasKey ? "Key in the Keychain (co.laris.oraclepet.paxa)" : "No key: the macOS voice speaks",
                      systemImage: hasKey ? "key.fill" : "key")
                    .font(.caption).foregroundStyle(hasKey ? .green : .secondary)
                if let e = PetVoice.lastPaxaError {
                    Text("Last Paxa error: \(e)").font(.caption).foregroundStyle(.orange)
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 460)
        .onAppear { hasKey = PaxaVoice.hasKey }
    }
}

/// One pet: its WiFi, and voice mode (the pet streams what it hears to the voice server).
private struct PetVoiceRow: View {
    @ObservedObject var link: PetLink

    var body: some View {
        HStack {
            Text(link.petName ?? "pet").font(.callout.monospaced())
            Text(link.wifiState ?? link.improvState ?? (link.isLinked ? "WiFi unknown" : "not linked"))
                .font(.caption).foregroundStyle(.secondary)
            Spacer()
            Toggle("Voice", isOn: Binding(get: { link.voiceOn }, set: { link.setVoice($0) }))
                .toggleStyle(.switch).controlSize(.small)
                .disabled(!link.isReady)
        }
        if let s = link.voiceState, link.voiceOn {
            Text("voice: \(s)").font(.caption2).foregroundStyle(.tertiary)
        }
    }
}
