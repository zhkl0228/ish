//
//  AgentActivity.swift
//  iSH
//
//  The app's side of the Live Activity (AgentActivityAttributes.swift), for
//  AgentActivityTracker.m: started as an agent starts working (ActivityKit
//  starts one only for an app in front), told of what changes, ended.
//

import ActivityKit
import Foundation

// Public, for iSH-Swift.h to have it: an app target's generated header has
// the internal declarations only with a bridging header, which the app has
// none of
@objc public final class AgentActivity: NSObject {
    private static var current: Activity<AgentActivityAttributes>?
    private static var shown: AgentActivityAttributes.ContentState?

    // What it shows: the activity told of it, or started if there is none
    // and a tab works (one stopped may be told of, others working)
    @objc public static func show(title: String, since: Date, stopped: Date?, workingCount: Int, stoppedCount: Int) {
        let state = AgentActivityAttributes.ContentState(title: title, since: since, stopped: stopped,
                                                         workingCount: workingCount, stoppedCount: stoppedCount)
        if let activity = current {
            if state == shown {
                return
            }
            shown = state
            Task {
                await activity.update(ActivityContent(state: state, staleDate: nil))
            }
            return
        }
        if workingCount == 0 {
            return
        }
        do {
            current = try Activity.request(attributes: AgentActivityAttributes(),
                                           content: ActivityContent(state: state, staleDate: nil))
            shown = state
        } catch {
            NSLog("AgentActivity: Live Activity not started: %@", String(describing: error))
        }
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
