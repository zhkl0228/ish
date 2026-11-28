//
//  AgentActivityTracker.m
//  iSH
//
//  The Live Activity (Dynamic Island, Lock Screen) of the tabs' agents:
//  started while one is in a tab (working, stopped or waiting) and the app is
//  in front, as ActivityKit requires; kept as the app comes to front, for an
//  agent that works again with the app away (2026-11-28: a background task
//  of Claude's done, it worked on) to be told of: there is no starting one
//  then. Told of the one that stopped last unseen, how long it worked, and
//  how many stopped and work, while one did, for it may be asking to be
//  allowed a tool, which matters more than the others' working (2026-11-28:
//  one asking showed as nothing while another worked); else of the one that
//  began working last, since when, and how many work; else of the one that
//  began waiting last, and how many wait. Ended once there is none.
//

#import <UIKit/UIKit.h>
#import "AgentActivityTracker.h"
#import "TerminalSession.h"
#import "iSH-Swift.h"
#include "debug.h"

// The sessions whose agent works, since when it does; those whose agent
// stopped unseen (agentStoppedUnseen), from when it worked till it stopped;
// and those whose agent waits, what it did seen, since when it does
static NSMutableDictionary<NSUUID *, NSDate *> *workingSince;
static NSMutableDictionary<NSUUID *, NSDateInterval *> *stoppedWork;
static NSMutableDictionary<NSUUID *, NSDate *> *waitingSince;
// Their agents' names, as they had one: one that stopped by exiting has none
// left to say (TerminalSession's agentName)
static NSMutableDictionary<NSUUID *, NSString *> *agentNames;
// Why ActivityKit last started none, said once till it says otherwise: tried
// again on each change with the app in front
static NSString *notStarted;

static void Publish(void) {
    if (workingSince.count == 0 && stoppedWork.count == 0 && waitingSince.count == 0) {
        [AgentActivity end];
        return;
    }
    if (!AgentActivity.started && UIApplication.sharedApplication.applicationState != UIApplicationStateActive)
        return;
    // The one told of: the one that stopped last, else the one that began
    // working last, else the one that began waiting last
    NSUUID *latest;
    AgentPhase phase;
    NSDate *since;
    NSDate *stopped;
    if (stoppedWork.count > 0) {
        latest = [stoppedWork keysSortedByValueUsingComparator:^NSComparisonResult(NSDateInterval *a, NSDateInterval *b) {
            return [a.endDate compare:b.endDate];
        }].lastObject;
        phase = AgentPhaseStopped;
        since = stoppedWork[latest].startDate;
        stopped = stoppedWork[latest].endDate;
    } else if (workingSince.count > 0) {
        latest = [workingSince keysSortedByValueUsingSelector:@selector(compare:)].lastObject;
        phase = AgentPhaseWorking;
        since = workingSince[latest];
    } else {
        latest = [waitingSince keysSortedByValueUsingSelector:@selector(compare:)].lastObject;
        phase = AgentPhaseWaiting;
        since = waitingSince[latest];
    }
    TerminalSession *told;
    for (TerminalSession *session in TerminalSessionStore.shared.sessions) {
        if ([session.uuid isEqual:latest])
            told = session;
    }
    NSString *failure = [AgentActivity showWithTitle:told.displayTitle
                                               agent:agentNames[latest]
                                               phase:phase
                                               since:since
                                             stopped:stopped
                                        workingCount:(NSInteger) workingSince.count
                                        stoppedCount:(NSInteger) stoppedWork.count
                                        waitingCount:(NSInteger) waitingSince.count];
    if (failure != nil && ![failure isEqualToString:notStarted])
        printk("live activity: not started: %s\n", failure.UTF8String);
    notStarted = failure;
}

static void Changed(TerminalSession *session) {
    NSUUID *uuid = session.uuid;
    BOOL running = session.state == TerminalSessionStateRunning;
    if (running && session.agentName != nil)
        agentNames[uuid] = session.agentName;
    if (running && session.agent == TerminalSessionAgentWorking) {
        if (workingSince[uuid] == nil)
            workingSince[uuid] = NSDate.date;
        [stoppedWork removeObjectForKey:uuid];
        [waitingSince removeObjectForKey:uuid];
        Publish();
        return;
    }
    // Set as it stops: agentStoppedUnseen is, as the change that it worked
    // no longer is announced
    NSDate *began = workingSince[uuid];
    [workingSince removeObjectForKey:uuid];
    if (running && session.agentStoppedUnseen) {
        if (stoppedWork[uuid] == nil)
            stoppedWork[uuid] = [[NSDateInterval alloc] initWithStartDate:began endDate:NSDate.date];
        [waitingSince removeObjectForKey:uuid];
    } else if (running && session.agent == TerminalSessionAgentWaiting) {
        [stoppedWork removeObjectForKey:uuid];
        if (waitingSince[uuid] == nil)
            waitingSince[uuid] = NSDate.date;
    } else {
        [stoppedWork removeObjectForKey:uuid];
        [waitingSince removeObjectForKey:uuid];
        [agentNames removeObjectForKey:uuid];
    }
    Publish();
}

void AgentActivityTrackerStart(void) {
    workingSince = [NSMutableDictionary new];
    stoppedWork = [NSMutableDictionary new];
    waitingSince = [NSMutableDictionary new];
    agentNames = [NSMutableDictionary new];
    // One of a run before this one, its tabs gone
    [AgentActivity endAll];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    // Posted on the main thread
    [center addObserverForName:TerminalSessionDidChangeNotification object:nil queue:nil
                    usingBlock:^(NSNotification *notification) {
        Changed(notification.object);
    }];
    // In front again: one started if there is none (ended by the system,
    // dismissed, none to start while the app was away)
    [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *_) {
        Publish();
    }];
}
