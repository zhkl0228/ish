//
//  AgentActivityTracker.m
//  iSH
//
//  The Live Activity (Dynamic Island, Lock Screen) of the tabs' Claude Code:
//  started as one starts working, with the app in front, as ActivityKit
//  requires; told which works, since when, and how many; once none works,
//  told of the one that stopped last and how long it worked, which shows
//  while the app is away; and
//  ended once none works and the app is in front, where the tabs show it.
//

#import <UIKit/UIKit.h>
#import "AgentActivityTracker.h"
#import "TerminalSession.h"
#import "iSH-Swift.h"

// The sessions whose agent works, since when it does; and those whose agent
// stopped unseen (agentStoppedUnseen), from when it worked till it stopped
static NSMutableDictionary<NSUUID *, NSDate *> *workingSince;
static NSMutableDictionary<NSUUID *, NSDateInterval *> *stoppedWork;

static void Publish(void) {
    BOOL inFront = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    if (workingSince.count == 0 && (stoppedWork.count == 0 || inFront)) {
        [AgentActivity end];
        return;
    }
    // The one told of: the one that began working last, else the one that
    // stopped last
    NSUUID *latest;
    NSDate *since;
    NSDate *stopped;
    if (workingSince.count > 0) {
        latest = [workingSince keysSortedByValueUsingSelector:@selector(compare:)].lastObject;
        since = workingSince[latest];
    } else {
        latest = [stoppedWork keysSortedByValueUsingComparator:^NSComparisonResult(NSDateInterval *a, NSDateInterval *b) {
            return [a.endDate compare:b.endDate];
        }].lastObject;
        since = stoppedWork[latest].startDate;
        stopped = stoppedWork[latest].endDate;
    }
    TerminalSession *told;
    for (TerminalSession *session in TerminalSessionStore.shared.sessions) {
        if ([session.uuid isEqual:latest])
            told = session;
    }
    [AgentActivity showWithTitle:told.displayTitle
                           since:since
                         stopped:stopped
                    workingCount:(NSInteger) workingSince.count
                    stoppedCount:(NSInteger) stoppedWork.count];
}

static void Changed(TerminalSession *session) {
    NSUUID *uuid = session.uuid;
    BOOL running = session.state == TerminalSessionStateRunning;
    if (running && session.agent == TerminalSessionAgentWorking) {
        if (workingSince[uuid] == nil)
            workingSince[uuid] = NSDate.date;
        [stoppedWork removeObjectForKey:uuid];
    } else {
        // Set as it stops: agentStoppedUnseen is, as the change that it
        // worked no longer is announced
        NSDate *began = workingSince[uuid];
        [workingSince removeObjectForKey:uuid];
        if (running && session.agentStoppedUnseen) {
            if (stoppedWork[uuid] == nil)
                stoppedWork[uuid] = [[NSDateInterval alloc] initWithStartDate:began endDate:NSDate.date];
        } else {
            [stoppedWork removeObjectForKey:uuid];
        }
    }
    Publish();
}

void AgentActivityTrackerStart(void) {
    workingSince = [NSMutableDictionary new];
    stoppedWork = [NSMutableDictionary new];
    // One of a run before this one, its tabs gone
    [AgentActivity endAll];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    // Posted on the main thread
    [center addObserverForName:TerminalSessionDidChangeNotification object:nil queue:nil
                    usingBlock:^(NSNotification *notification) {
        Changed(notification.object);
    }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *_) {
        Publish();
    }];
}
