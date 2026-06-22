import SwiftUI

struct PresetNavigationView: View {
    @Environment(AppState.self) var appState
    @Environment(PresetLibrary.self) var presetLibrary

    private var previousPreset: Preset? {
        guard let current = appState.currentPreset else { return nil }
        let pc = current.midiProgramNumber
        guard pc > 0 else { return nil }
        return presetLibrary.preset(for: pc - 1)
    }

    private var nextPreset: Preset? {
        guard let current = appState.currentPreset else { return nil }
        let pc = current.midiProgramNumber
        guard pc < 127 else { return nil }
        return presetLibrary.preset(for: pc + 1)
    }

    var body: some View {
        HStack {
            if let prev = previousPreset {
                Button {
                    Task { await appState.selectPreset(prev) }
                } label: {
                    Label(prev.name, systemImage: "chevron.left")
                        .lineLimit(1)
                }
            }

            Spacer()

            if let next = nextPreset {
                Button {
                    Task { await appState.selectPreset(next) }
                } label: {
                    Label(next.name, systemImage: "chevron.right")
                        .labelStyle(.trailingIcon)
                        .lineLimit(1)
                }
            }
        }
        .padding()
    }
}

private struct TrailingIconLabelStyle: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack {
            configuration.title
            configuration.icon
        }
    }
}

extension LabelStyle where Self == TrailingIconLabelStyle {
    static var trailingIcon: TrailingIconLabelStyle { TrailingIconLabelStyle() }
}
