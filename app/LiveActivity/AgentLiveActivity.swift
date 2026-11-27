//
//  AgentLiveActivity.swift
//  iSH
//
//  The Live Activity extension: the tabs' Claude Code on the Dynamic Island
//  and the Lock Screen (AgentActivityAttributes.swift). At work, sparkles and
//  the time it has worked; stopped, a green check.
//

import ActivityKit
import SwiftUI
import WidgetKit

@main
struct AgentWidgets: WidgetBundle {
    var body: some Widget {
        AgentLiveActivity()
    }
}

extension AgentActivityAttributes.ContentState {
    var status: String {
        if working {
            return workingCount > 1 ? "Claude is working in \(workingCount) tabs" : "Claude is working"
        }
        return stoppedCount > 1 ? "Claude stopped in \(stoppedCount) tabs" : "Claude stopped"
    }
}

struct StateIcon: View {
    let state: AgentActivityAttributes.ContentState

    var body: some View {
        if state.working {
            Image(systemName: "sparkles").foregroundStyle(.orange)
        } else {
            Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        }
    }
}

struct AgentLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: AgentActivityAttributes.self) { context in
            HStack(spacing: 12) {
                StateIcon(state: context.state).font(.title2)
                VStack(alignment: .leading, spacing: 2) {
                    Text(context.state.title).font(.headline).lineLimit(1)
                    Text(context.state.status).font(.subheadline).foregroundStyle(.secondary)
                }
                Spacer(minLength: 8)
                if context.state.working {
                    Text(context.state.since, style: .timer)
                        .font(.headline)
                        .monospacedDigit()
                        .multilineTextAlignment(.trailing)
                        .frame(width: 72, alignment: .trailing)
                }
            }
            .padding(16)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    StateIcon(state: context.state).font(.title2)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    if context.state.working {
                        Text(context.state.since, style: .timer)
                            .monospacedDigit()
                            .multilineTextAlignment(.trailing)
                            .frame(width: 64, alignment: .trailing)
                    }
                }
                DynamicIslandExpandedRegion(.bottom) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(context.state.title).font(.headline).lineLimit(1)
                        Text(context.state.status).font(.subheadline).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            } compactLeading: {
                StateIcon(state: context.state)
            } compactTrailing: {
                if context.state.working {
                    Text(context.state.since, style: .timer)
                        .monospacedDigit()
                        .multilineTextAlignment(.trailing)
                        .frame(width: 44, alignment: .trailing)
                }
            } minimal: {
                StateIcon(state: context.state)
            }
        }
    }
}
