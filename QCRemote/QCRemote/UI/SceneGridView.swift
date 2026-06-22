import SwiftUI

struct SceneGridView: View {
    @Environment(AppState.self) var appState
    @Environment(\.horizontalSizeClass) var horizontalSize

    var body: some View {
        let columns = horizontalSize == .compact
            ? [GridItem(), GridItem()]
            : [GridItem(), GridItem(), GridItem(), GridItem()]

        LazyVGrid(columns: columns, spacing: 12) {
            ForEach(0..<8, id: \.self) { index in
                SceneButton(index: index)
            }
        }
        .padding()
    }
}

struct SceneButton: View {
    let index: Int
    @Environment(AppState.self) var appState

    private var scene: PresetScene? {
        appState.currentPreset?.scenes.first { $0.index == index }
    }

    private var isActive: Bool {
        appState.currentSceneIndex == index
    }

    var body: some View {
        Button {
            Task { await appState.selectScene(index) }
        } label: {
            VStack {
                Text(String(index + 1))
                    .font(.caption)
                Text(scene?.name ?? "Scene \(index + 1)")
                    .font(.caption2)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity)
            .frame(height: 60)
            .background(isActive ? Color.blue : Color.gray.opacity(0.3))
            .foregroundStyle(.white)
            .cornerRadius(8)
        }
    }
}
