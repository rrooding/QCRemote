import SwiftUI

struct PresetHeaderView: View {
    @Environment(AppState.self) var appState

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Circle()
                    .fill(appState.midiConnectionStatus == .connected ? .green : .red)
                    .frame(width: 10, height: 10)

                if let preset = appState.currentPreset {
                    VStack(alignment: .leading) {
                        Text(preset.bankAndSlot)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(preset.name)
                            .font(.title2)
                            .bold()
                    }
                } else {
                    Text("No preset selected")
                        .font(.title2)
                        .foregroundStyle(.secondary)
                }
            }

            if appState.isLastKnownState, appState.currentPreset != nil {
                Label("Last known", systemImage: "clock.badge.questionmark.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
        }
        .padding()
    }
}
