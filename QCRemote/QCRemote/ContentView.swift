//
//  ContentView.swift
//  QCRemote
//
//  Created by Ralph Rooding on 22/06/2026.
//

import SwiftUI

struct ContentView: View {
    @Environment(AppState.self) var appState
    @State private var showSettings = false

    var body: some View {
        VStack(spacing: 0) {
            PresetHeaderView()
            Divider()
            SceneGridView()
            Spacer()
            PresetNavigationView()
        }
        .toolbar {
            ToolbarItem(placement: .automatic) {
                Button {
                    showSettings = true
                } label: {
                    Label("Settings", systemImage: "gear")
                }
            }
        }
        .sheet(isPresented: $showSettings) {
            SettingsSheet()
        }
    }
}
