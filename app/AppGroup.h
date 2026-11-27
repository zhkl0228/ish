//
//  AppGroup.h
//  iSH
//
//  Created by Theodore Dubois on 2/28/20.
//

#import <Foundation/Foundation.h>

NSURL *ContainerURL(void);
// The app groups the running binary is signed with
// (com.apple.security.application-groups), nil if none
NSArray<NSString *> *CurrentAppGroups(void);
