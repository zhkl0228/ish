//
//  AgentLiveActivity.swift
//  iSH
//
//  The Live Activity extension: the tabs' agents (Claude Code, Kimi Code) on
//  the Dynamic Island and the Lock Screen (AgentActivityAttributes.swift). At
//  work, sparkles and the time it has worked; stopped, a green check and the
//  time it worked; waiting, grey sparkles.
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
    // The one told of, by its agent's name (tabs may run different agents),
    // then how many more do as it does, and how many work if it stopped:
    // "Kimi stopped · 1 more stopped · 2 working", "Claude is working · 1
    // more working", "Claude is waiting · 1 more waiting". One waiting is
    // told of only while none works or stopped unseen.
    var status: String {
        switch phase {
        case .working:
            return joined("\(agent) is working", more: workingCount, "working")
        case .stopped:
            var status = joined("\(agent) stopped", more: stoppedCount, "stopped")
            if workingCount > 0 {
                status += " · \(workingCount) working"
            }
            return status
        case .waiting:
            return joined("\(agent) is waiting", more: waitingCount, "waiting")
        }
    }

    private func joined(_ first: String, more count: Int, _ doing: String) -> String {
        count > 1 ? "\(first) · \(count - 1) more \(doing)" : first
    }
}

struct StateIcon: View {
    let state: AgentActivityAttributes.ContentState

    var body: some View {
        switch state.phase {
        case .working:
            Image(systemName: "sparkles").foregroundStyle(.orange)
        case .stopped:
            Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .waiting:
            Image(systemName: "sparkles").foregroundStyle(.secondary)
        }
    }
}

// The time it has worked, running; once it stopped, the time it worked, as
// it was then (2026-11-28: on a foldable's upright island the check alone
// left the trailing half empty); waiting, none
struct WorkedTime: View {
    let state: AgentActivityAttributes.ContentState

    var body: some View {
        switch state.phase {
        case .working:
            Text(state.since, style: .timer)
        case .stopped:
            let stopped = state.stopped!
            Text(timerInterval: state.since...stopped, pauseTime: stopped, countsDown: false)
                .foregroundStyle(.secondary)
        case .waiting:
            EmptyView()
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
