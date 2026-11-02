/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * macOS application menu (BL-022, ABOUT-001)
 * ==========================================
 *
 * SDL creates the native application menu with an "About <app>" item that opens the system's
 * standard About panel. This retargets that item to Lume's own About (the same instance as Help >
 * About Lume Data and the menu-bar brand): the item posts an SDL user event, which the main loop
 * turns into about_open. Nothing else in the menu changes.
 */
#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include "macos_app.h"

static Uint32 about_event;

@interface LumeAboutTarget : NSObject
- (void)showLumeAbout:(id)sender;
@end
@implementation LumeAboutTarget
- (void)showLumeAbout:(id)sender {
    (void)sender;
    SDL_Event ev; SDL_zero(ev); ev.type = about_event;
    SDL_PushEvent(&ev);   /* handled on the main loop, like every other action */
}
@end

@interface LumeSettingsTarget : NSObject
- (void)showLumeSettings:(id)sender;
@end
static Uint32 settings_event;
@implementation LumeSettingsTarget
- (void)showLumeSettings:(id)sender {
    (void)sender;
    SDL_Event ev; SDL_zero(ev); ev.type = settings_event;
    SDL_PushEvent(&ev);
}
@end

static LumeAboutTarget *target;
static NSMenuItem *about_item;

bool macos_route_about(Uint32 event_type, const char *product_name) {
    about_event = event_type;
    if (!target) target = [[LumeAboutTarget alloc] init];
    NSMenu *main = [NSApp mainMenu];
    if (!main || [main numberOfItems] < 1) return false;
    NSMenu *app = [[main itemAtIndex:0] submenu];
    for (NSMenuItem *it in [app itemArray]) {
        if ([it action] == @selector(orderFrontStandardAboutPanel:) || [it action] == @selector(showLumeAbout:)) {
            [it setTarget:target];
            [it setAction:@selector(showLumeAbout:)];
            [it setTitle:[NSString stringWithFormat:@"About %s", product_name]];
            about_item = it;
            return true;
        }
    }
    return false;
}

bool macos_trigger_about(void) {
    /* what a click on the native item does (test harness) */
    if (!about_item) return false;
    return [NSApp sendAction:[about_item action] to:[about_item target] from:about_item];
}

const char *macos_about_title(void) { return about_item ? [[about_item title] UTF8String] : ""; }

/* the native "Settings..." item (Cmd+,): SDL leaves it without an action (greyed out); it opens Lume's Settings */
static LumeSettingsTarget *s_target;
static NSMenuItem *settings_item;
bool macos_route_settings(Uint32 event_type) {
    settings_event = event_type;
    if (!s_target) s_target = [[LumeSettingsTarget alloc] init];
    NSMenu *main = [NSApp mainMenu];
    if (!main || [main numberOfItems] < 1) return false;
    NSMenu *app = [[main itemAtIndex:0] submenu];
    for (NSMenuItem *it in [app itemArray]) {
        NSString *t = [it title];
        if ([[it keyEquivalent] isEqualToString:@","] || [t hasPrefix:@"Settings"] || [t hasPrefix:@"Preferences"]) {
            [it setTarget:s_target];
            [it setAction:@selector(showLumeSettings:)];
            [it setEnabled:YES];
            settings_item = it;
            return true;
        }
    }
    return false;
}
bool macos_trigger_settings(void) {
    if (!settings_item) return false;
    return [NSApp sendAction:[settings_item action] to:[settings_item target] from:settings_item];
}
