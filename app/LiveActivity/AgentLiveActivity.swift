//
//  AgentLiveActivity.swift
//  iSH
//
//  The Live Activity extension: the tabs' Claude Code on the Dynamic Island
//  and the Lock Screen (AgentActivityAttributes.swift). At work, sparkles and
//  the time it has worked; stopped, a green check and the time it worked.
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
    // The one stopped, told of before those working, with how many work
    var status: String {
        if working {
            return workingCount > 1 ? "Claude is working in \(workingCount) tabs" : "Claude is working"
        }
        let stopped = stoppedCount > 1 ? "Claude stopped in \(stoppedCount) tabs" : "Claude stopped"
        return workingCount > 0 ? "\(stopped) · \(workingCount) working" : stopped
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

// The time it has worked, running; once it stopped, the time it worked, as
// it was then (2026-11-28: on a foldable's upright island the check alone
// left the trailing half empty)
struct WorkedTime: View {
    let state: AgentActivityAttributes.ContentState

    var body: some View {
        if let stopped = state.stopped {
            Text(timerInterval: state.since...stopped, pauseTime: stopped, countsDown: false)
                .foregroundStyle(.secondary)
        } else {
            Text(state.since, style: .timer)
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
                WorkedTime(state: context.state)
                    .font(.headline)
                    .monospacedDigit()
                    .multilineTextAlignment(.trailing)
                    .frame(width: 72, alignment: .trailing)
            }
            .padding(16)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    StateIcon(state: context.state).font(.title2)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    WorkedTime(state: context.state)
                        .monospacedDigit()
                        .multilineTextAlignment(.trailing)
                        .frame(width: 64, alignment: .trailing)
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
                // At most as wide as the region, which on a foldable's
                // island, upright at the screen's side, is narrower than the
                // 44 points it was given: 2026-11-27, folded, "0:5" and the
                // rest cut off. Small, to fit there.
                WorkedTime(state: context.state)
                    .font(.caption.monospacedDigit())
                    .lineLimit(1)
                    .minimumScaleFactor(0.5)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 44)
            } minimal: {
                StateIcon(state: context.state)
            }
        }
    }
}
