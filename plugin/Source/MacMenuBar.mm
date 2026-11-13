#include "MacMenuBar.h"
#import <AppKit/AppKit.h>

// Attach a native menu so VoiceOver can activate the status item, as in Sound Canvas Bridge.
@interface HS2MenuTarget : NSObject <NSApplicationDelegate> {
@public
    std::function<void()> show;
    std::function<void()> preferences;
    id<NSApplicationDelegate> previous;
}
- (void)showControls:(id)sender;
- (void)showPreferences:(id)sender;
- (void)quit:(id)sender;
@end

@implementation HS2MenuTarget
- (void)showControls:(id)sender { juce::ignoreUnused(sender); show(); }
- (void)showPreferences:(id)sender { juce::ignoreUnused(sender); preferences(); }
- (void)quit:(id)sender {
    juce::ignoreUnused(sender);
    if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
}
- (BOOL)applicationShouldHandleReopen:(NSApplication*)app hasVisibleWindows:(BOOL)visible {
    juce::ignoreUnused(app, visible);
    show();
    return NO;
}
- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [previous respondsToSelector:selector];
}
- (id)forwardingTargetForSelector:(SEL)selector {
    if ([previous respondsToSelector:selector]) return previous;
    return [super forwardingTargetForSelector:selector];
}
@end

namespace hs2port {
namespace {
class MacMenuBarItem final : public juce::SystemTrayIconComponent {
public:
    MacMenuBarItem(std::function<void()> show, std::function<void()> preferences) {
        juce::Image icon(juce::Image::ARGB, 20, 20, true);
        juce::Graphics g(icon);
        g.setColour(juce::Colours::black);
        for (int i = 0; i < 3; ++i) g.fillRect(3 + 5 * i, 5 + 2 * i, 3, 12 - 2 * i);
        setIconImage(icon, icon);
        auto* item = static_cast<NSStatusItem*>(getNativeHandle());
        if (!item.button) return;
        [item.button setAccessibilityLabel:@"HS2 Port"];
        item.button.toolTip = @"HS2 Port";
        target = [[HS2MenuTarget alloc] init];
        target->show = std::move(show);
        target->preferences = std::move(preferences);
        target->previous = [NSApp.delegate retain];
        NSApp.delegate = target;
        NSMenu* menu = [[[NSMenu alloc] initWithTitle:@"HS2 Port"] autorelease];
        auto add = [&](NSString* label, SEL action) {
            NSMenuItem* entry = [menu addItemWithTitle:label action:action keyEquivalent:@""];
            entry.target = target;
        };
        add(@"Show Controls", @selector(showControls:));
        add(@"Preferences...", @selector(showPreferences:));
        add(@"Quit HS2 Port", @selector(quit:));
        item.menu = menu;
    }
    ~MacMenuBarItem() override {
        if (!target) return;
        auto* item = static_cast<NSStatusItem*>(getNativeHandle());
        item.menu = nil;
        if (NSApp.delegate == target) NSApp.delegate = target->previous;
        [target->previous release];
        [target release];
    }
private:
    HS2MenuTarget* target = nil;
};
}

std::unique_ptr<juce::SystemTrayIconComponent> createMacMenuBarItem(std::function<void()> show,
                                                                  std::function<void()> preferences) {
    return std::make_unique<MacMenuBarItem>(std::move(show), std::move(preferences));
}

void setMacMenuBarHidden(bool hidden) {
    [NSApp setActivationPolicy:hidden ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
    if (!hidden) [NSApp activateIgnoringOtherApps:YES];
}
}
