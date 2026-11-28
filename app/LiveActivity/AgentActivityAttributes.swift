//
//  AgentActivityAttributes.swift
//  iSH
//
//  The Live Activity of the tabs' agents (Claude Code, Kimi Code;
//  TerminalSession's agent): what the app tells it (AgentActivity.swift) and
//  the extension shows of it (AgentLiveActivity.swift), on the Dynamic Island
//  and the Lock Screen. Compiled into both.
//

import ActivityKit
import Foundation

// What the agent told of does: works; stopped working, not yet seen (done, or
// asking to be allowed a tool); or waits for the person, what it did seen.
// Public and @objc for AgentActivityTracker.m to tell it, through
// iSH-Swift.h.
@objc public enum AgentPhase: Int, Codable {
    case working
    case stopped
    case waiting
}

struct AgentActivityAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        // The tab told of: the one that stopped last unseen, else the one
        // that began working last, else the one that began waiting last
        var title: String
        // Its agent's name (Claude, Kimi)
        var agent: String
        var phase: AgentPhase
        // When it began working (working, stopped) or waiting (waiting), and
        // when it stopped (stopped; nil else)
        var since: Date
        var stopped: Date?
        // How many tabs' agents work, how many stopped unseen, and how many
        // wait
        var workingCount: Int
        var stoppedCount: Int
        var waitingCount: Int
    }
}
