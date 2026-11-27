//
//  AgentActivityTracker.m
//  iSH
//
//  The Live Activity (Dynamic Island, Lock Screen) of the tabs' Claude Code:
//  started as one starts working, with the app in front, as ActivityKit
//  requires; told which works, since when, and how many; once none works,
//  told of the one that stopped last, which shows while the app is away; and
//  ended once none works and the app is in front, where the tabs show it.
//

#import <UIKit/UIKit.h>
#import "AgentActivityTracker.h"
#import "TerminalSession.h"
#import "iSH-Swift.h"

// The sessions whose agent works, since when it does; and those whose agent
// stopped unseen (agentStoppedUnseen), when it did
static NSMutableDictionary<NSUUID *, NSDate *> *workingSince;
static NSMutableDictionary<NSUUID *, NSDate *> *stoppedAt;

static void Publish(void) {
    BOOL inFront = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    if (workingSince.count == 0 && (stoppedAt.count == 0 || inFront)) {
        [AgentActivity end];
        return;
    }
    // The one told of: the one that began working last, else the one that
    // stopped last
    NSDictionary<NSUUID *, NSDate *> *times = workingSince.count > 0 ? workingSince : stoppedAt;
    NSUUID *latest = [times keysSortedByValueUsingSelector:@selector(compare:)].lastObject;
    TerminalSession *told;
    for (TerminalSession *session in TerminalSessionStore.shared.sessions) {
        if ([session.uuid isEqual:latest])
            told = session;
    }
    [AgentActivity showWithTitle:told.displayTitle
                         working:workingSince.count > 0
                           since:times[latest]
                    workingCount:(NSInteger) workingSince.count
                    stoppedCount:(NSInteger) stoppedAt.count];
}

static void Changed(TerminalSession *session) {
    NSUUID *uuid = session.uuid;
    BOOL running = session.state == TerminalSessionStateRunning;
    if (running && session.agent == TerminalSessionAgentWorking) {
        if (workingSince[uuid] == nil)
            workingSince[uuid] = NSDate.date;
        [stoppedAt removeObjectForKey:uuid];
    } else {
        [workingSince removeObjectForKey:uuid];
        if (running && session.agentStoppedUnseen) {
            if (stoppedAt[uuid] == nil)
                stoppedAt[uuid] = NSDate.date;
        } else {
            [stoppedAt removeObjectForKey:uuid];
        }
    }
    Publish();
}

void AgentActivityTrackerStart(void) {
    workingSince = [NSMutableDictionary new];
    stoppedAt = [NSMutableDictionary new];
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
