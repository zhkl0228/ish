//
//  AgentActivity.swift
//  iSH
//
//  The app's side of the Live Activity (AgentActivityAttributes.swift), for
//  AgentActivityTracker.m: started while an agent is in a tab and the app in
//  front (ActivityKit starts one only for an app in front), told of what
//  changes, ended.
//

import ActivityKit
import Foundation

// Public, for iSH-Swift.h to have it: an app target's generated header has
// the internal declarations only with a bridging header, which the app has
// none of
@objc public final class AgentActivity: NSObject {
    private static var current: Activity<AgentActivityAttributes>?
    private static var shown: AgentActivityAttributes.ContentState?

    // There is one: it can be told of what changes, the app in front or not
    @objc public static var started: Bool {
        current != nil
    }

    // What it shows: the activity told of it, or started if there is none.
    // nil once it is, else why ActivityKit started none.
    @objc public static func show(title: String, agent: String, phase: AgentPhase, since: Date, stopped: Date?,
                                  workingCount: Int, stoppedCount: Int, waitingCount: Int) -> String? {
        let state = AgentActivityAttributes.ContentState(title: title, agent: agent, phase: phase, since: since,
                                                         stopped: stopped, workingCount: workingCount,
                                                         stoppedCount: stoppedCount, waitingCount: waitingCount)
        if let activity = current {
            if state == shown {
                return nil
            }
            shown = state
            Task {
                await activity.update(ActivityContent(state: state, staleDate: nil))
            }
            return nil
        }
        let activity: Activity<AgentActivityAttributes>
        do {
            activity = try Activity.request(attributes: AgentActivityAttributes(),
                                            content: ActivityContent(state: state, staleDate: nil))
        } catch {
            return String(describing: error)
        }
        current = activity
        shown = state
        // Ended by the system (at most eight hours on), or dismissed on the
        // Lock Screen: none left to tell of what changes, the next one
        // started with the app in front again
        Task { @MainActor in
            for await state in activity.activityStateUpdates where state == .ended || state == .dismissed {
                if current?.id == activity.id {
                    current = nil
                    shown = nil
                }
                return
            }
        }
        return nil
    }

    @objc public static func end() {
        guard let activity = current else {
            return
        }
        current = nil
        shown = nil
        Task {
            await activity.end(nil, dismissalPolicy: .immediate)
        }
    }

    // Each of the app's, those of a run before this one's among them: their
    // tabs are gone
    @objc public static func endAll() {
        current = nil
        shown = nil
        for activity in Activity<AgentActivityAttributes>.activities {
            Task {
                await activity.end(nil, dismissalPolicy: .immediate)
            }
        }
    }
}
