//
//  AgentActivityAttributes.swift
//  iSH
//
//  The Live Activity of the tabs' Claude Code (TerminalSession's agent): what
//  the app tells it (AgentActivity.swift) and the extension shows of it
//  (AgentLiveActivity.swift), on the Dynamic Island and the Lock Screen.
//  Compiled into both.
//

import ActivityKit
import Foundation

struct AgentActivityAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        // The tab told of: the one that stopped last unseen, else the one
        // that began working last
        var title: String
        // When it began working, and when it stopped (done, or asking to be
        // allowed a tool), nil while it works
        var since: Date
        var stopped: Date?
        // How many tabs' agents work, and how many stopped unseen
        var workingCount: Int
        var stoppedCount: Int

        var working: Bool {
            stopped == nil
        }
    }
}
