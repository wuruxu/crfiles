// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_mac.h"
#include "crFiles/crfiles_window.h"

#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>

#include <memory>
#include <atomic>
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include <algorithm>
#include <cmath>

#include "crFiles/crfiles_frame_view.h"
#include "crFiles/crfiles_settings_dialog.h"
#include "crFiles/grit/crfiles_strings.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "ui/base/l10n/l10n_util_mac.h"

#include "base/apple/foundation_util.h"
#include "base/mac/mac_util.h"
#include "base/strings/sys_string_conversions.h"
#include "base/synchronization/lock.h"
#include "skia/ext/skia_utils_mac.h"

#include "base/check_op.h"
#include "components/remote_cocoa/common/native_widget_ns_window.mojom.h"
#include "ui/menus/cocoa/menu_controller.h"
#include "ui/gfx/mac/coordinate_conversion.h"
#include "ui/gfx/image/image_skia_util_mac.h"
#include "ui/views/widget/native_widget_mac.h"

// NSMenuItem does not retain its target. Keep this object alive until the
// application menu is removed, and keep only a weak reference to the window.
@interface CrFilesApplicationMenuTarget : NSObject <NSMenuItemValidation, NSApplicationDelegate>
- (instancetype)initWithWindow:(views::Widget*)window;
- (void)showAbout:(id)sender;
- (void)showSettings:(id)sender;
- (void)quit:(id)sender;
@end

@implementation CrFilesApplicationMenuTarget {
  base::WeakPtr<views::Widget> _window;
}
- (instancetype)initWithWindow:(views::Widget*)window {
  if ((self = [super init])) _window = window->GetWeakPtr();
  return self;
}
- (BOOL)validateMenuItem:(NSMenuItem*)item {
  return _window && !_window->IsClosed();
}
- (void)showAbout:(id)sender {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<views::Widget> window) {
        if (window && !window->IsClosed())
          crfiles::ShowCrFilesAboutDialog(window.get());
      }, _window));
}
- (void)showSettings:(id)sender {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<views::Widget> window) {
        if (window && !window->IsClosed())
          crfiles::ShowCrFilesSettingsDialog(window.get());
      }, _window));
}
- (void)quit:(id)sender {
  // Exit explicitly; closing the main window may only hide it in the tray.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<views::Widget> window) {
        if (window && !window->IsClosed()) crfiles::QuitCrFiles();
      }, _window));
}
- (BOOL)applicationShouldHandleReopen:(NSApplication*)application hasVisibleWindows:(BOOL)visible {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&crfiles::RestoreCrFilesWindow));
  return YES;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)application {
  return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&crfiles::QuitCrFiles));
  return NSTerminateCancel;
}
@end

namespace crfiles {
namespace {
CrFilesApplicationMenuTarget* g_application_menu_target = nil;
NSMenu* g_application_menu = nil;

class CrFilesNativeWidgetMac : public views::NativeWidgetMac {
 public:
  explicit CrFilesNativeWidgetMac(
      views::internal::NativeWidgetDelegate* delegate)
      : views::NativeWidgetMac(delegate) {}

 protected:
  void GetWindowFrameTitlebarHeight(bool* override_titlebar_height,
                                   float* titlebar_height) override {
    // BrowserWindowFrame centers AppKit's traffic lights in the tab row and
    // handles reverting to the system titlebar height in content fullscreen.
    *override_titlebar_height = true;
    *titlebar_height = kCrFilesTitlebarHeight;
  }

  void PopulateCreateWindowParams(
      const views::Widget::InitParams& widget_params,
      remote_cocoa::mojom::CreateWindowParams* params) override {
    views::NativeWidgetMac::PopulateCreateWindowParams(widget_params, params);
    // Both styles extend the tab row into the titlebar. The browser frame
    // preserves real AppKit controls; the frameless variant hides them for
    // the opt-in Windows-style caption buttons.
    params->window_class = widget_params.remove_standard_frame
        ? remote_cocoa::mojom::WindowClass::kFrameless
        : remote_cocoa::mojom::WindowClass::kBrowser;
    params->style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                         NSWindowStyleMaskMiniaturizable |
                         NSWindowStyleMaskResizable |
                         NSWindowStyleMaskFullSizeContentView;
    params->window_title_hidden = true;
    params->titlebar_appears_transparent = true;
  }
};

}  // namespace

gfx::Rect WindowBoundsForContentMac(views::Widget* widget,
                                    const gfx::Rect& content_bounds) {
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  return gfx::ScreenRectFromNSRect([window
      frameRectForContentRect:gfx::ScreenRectToNSRect(content_bounds)]);
}

int WindowControlsRightEdgeMac(views::Widget* widget) {
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  NSView* content = window.contentView;
  CGFloat right = 0;
  for (NSWindowButton type : {NSWindowCloseButton, NSWindowMiniaturizeButton,
                             NSWindowZoomButton}) {
    NSButton* button = [window standardWindowButton:type];
    if (button && button.superview && content) {
      NSRect bounds = [content convertRect:button.bounds fromView:button];
      right = std::max(right, NSMaxX(bounds));
    }
  }
  // Reserve space even before AppKit finishes the initial control layout.
  return std::max(96, static_cast<int>(std::ceil(right)) + 12);
}

views::NativeWidget* CreateCrFilesNativeWidgetMac(
    views::internal::NativeWidgetDelegate* delegate) {
  return new CrFilesNativeWidgetMac(delegate);
}

void InitializeCrFilesApplicationMac() {
  // Enable native context-menu vector icons on macOS 26 and later, while
  // preserving the legacy menu behavior on older systems.
  [MenuControllerCocoa
      initializeWithNewMenuIconScheme:base::mac::MacOSMajorVersion() >= 26];

  // Like Views Examples, support launching an unbundled binary from Terminal.
  ProcessSerialNumber process = {0, kCurrentProcess};
  CHECK_EQ(TransformProcessType(&process,
                               kProcessTransformToForegroundApplication),
           noErr);
}

void InstallCrFilesApplicationMenuMac(views::Widget* window) {
  g_application_menu_target = [[CrFilesApplicationMenuTarget alloc] initWithWindow:window];
  if (!NSApp.delegate) NSApp.delegate = g_application_menu_target;
  g_application_menu = [[NSMenu alloc] initWithTitle:@""];
  NSMenuItem* app_item = [[NSMenuItem alloc] initWithTitle:@"crFiles" action:nil keyEquivalent:@""];
  NSMenu* app_menu = [[NSMenu alloc] initWithTitle:@"crFiles"];
  auto add = [&](NSString* title, SEL action, NSString* key, id target) {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.target = target;
    item.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [app_menu addItem:item];
  };
  add(l10n_util::GetNSString(IDS_CRFILES_MAC_ABOUT), @selector(showAbout:), @"", g_application_menu_target);
  [app_menu addItem:NSMenuItem.separatorItem];
  add(l10n_util::GetNSString(IDS_CRFILES_MAC_SETTINGS), @selector(showSettings:), @",", g_application_menu_target);
  [app_menu addItem:NSMenuItem.separatorItem];
  add(l10n_util::GetNSString(IDS_CRFILES_MAC_HIDE), @selector(hide:), @"h", NSApp);
  [app_menu addItem:NSMenuItem.separatorItem];
  add(l10n_util::GetNSString(IDS_CRFILES_MAC_QUIT), @selector(quit:), @"q", g_application_menu_target);
  app_item.submenu = app_menu;
  [g_application_menu addItem:app_item];
  NSApp.mainMenu = g_application_menu;
}
void ActivateCrFilesApplicationMac() {
  [NSApp unhide:nil];
  [NSApp activateIgnoringOtherApps:YES];
}
bool IsCrFilesApplicationHiddenMac() { return NSApp.isHidden; }
void PrepareCrFilesShareBubbleMac(views::Widget* widget, bool background) {
  NSWindow* native = widget->GetNativeWindow().GetNativeNSWindow();
  native.hidesOnDeactivate = NO;
  auto behavior = native.collectionBehavior;
  behavior &= ~NSWindowCollectionBehaviorFullScreenPrimary;
  behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
  if (background) behavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
  else behavior &= ~NSWindowCollectionBehaviorCanJoinAllSpaces;
  if (native.collectionBehavior != behavior) native.collectionBehavior = behavior;
  if (background && NSApp.isHidden) {
    // Cmd+H hides the application without updating Views' window visibility.
    // Hide its main window explicitly before unhiding only the independent UI.
    HideCrFilesWindow();
    [NSApp unhideWithoutActivation];
  }
}

void RemoveCrFilesApplicationMenuMac() {
  if (NSApp.delegate == g_application_menu_target) NSApp.delegate = nil;
  if (NSApp.mainMenu == g_application_menu) NSApp.mainMenu = nil;
  g_application_menu = nil;
  g_application_menu_target = nil;
}

base::ScopedClosureRunner ObserveTitlebarClicksMac(
    views::Widget* window, base::RepeatingClosure callback) {
  const auto weak_window = window->GetWeakPtr();
  id monitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown |
                                           NSEventMaskRightMouseDown)
      handler:^NSEvent*(NSEvent* event) {
        if (!weak_window || weak_window->IsClosed() ||
            weak_window->IsFullscreen()) return event;
        NSWindow* native = weak_window->GetNativeWindow().GetNativeNSWindow();
        if (event.window != native || native.attachedSheet) return event;
        NSView* content = native.contentView;
        if (!content) return event;
        const NSPoint point = [content convertPoint:event.locationInWindow fromView:nil];
        const CGFloat top = content.isFlipped ? point.y - NSMinY(content.bounds)
                                             : NSMaxY(content.bounds) - point.y;
        if (point.x >= NSMinX(content.bounds) && point.x < NSMaxX(content.bounds) &&
            top >= 0 && top < kCrFilesTitlebarHeight) {
          // The caller defers view changes until native event dispatch unwinds.
          callback.Run();
        }
        return event;
      }];
  return base::ScopedClosureRunner(base::BindOnce([](id token) {
    if (token) [NSEvent removeMonitor:token];
  }, monitor));
}

gfx::ImageSkia LoadApplicationIconMac(const base::FilePath& path, int edge) {
  @autoreleasepool {
    NSImage* icon = [NSWorkspace.sharedWorkspace
        iconForFile:base::SysUTF8ToNSString(path.value())];
    if (!icon || edge <= 0) return {};
    auto image = gfx::ImageSkiaFromResizedNSImage(icon, NSMakeSize(edge, edge));
    if (!image.isNull()) image.MakeThreadSafe();
    return image;
  }
}

bool OpenFileWithMac(const base::FilePath& path, const std::string& application) {
  CHECK(!NSThread.isMainThread);
  struct LaunchResult {
    base::WaitableEvent done;
    std::atomic_bool succeeded{false};
  };
  auto result = std::make_shared<LaunchResult>();
  NSURL* url = base::apple::FilePathToNSURL(path);
  NSURL* app = nil;
  if (application == "Terminal")
    app = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.Terminal"];
  else if (!application.empty()) {
    if (!base::FilePath(application).IsAbsolute()) return false;
    app = base::apple::FilePathToNSURL(base::FilePath(application));
  }
  if (!application.empty() && !app) return false;
  auto completion = ^(NSRunningApplication* launched, NSError* error) {
    result->succeeded.store(launched != nil && error == nil);
    result->done.Signal();
  };
  if (app) [NSWorkspace.sharedWorkspace openURLs:@[url] withApplicationAtURL:app
      configuration:NSWorkspaceOpenConfiguration.configuration completionHandler:completion];
  else [NSWorkspace.sharedWorkspace openURL:url
      configuration:NSWorkspaceOpenConfiguration.configuration completionHandler:completion];
  return result->done.TimedWait(base::Seconds(10)) && result->succeeded.load();
}
bool OpenProjectURLMac(const std::string& url) {
  NSURL* target = [NSURL URLWithString:base::SysUTF8ToNSString(url)];
  return target && [NSWorkspace.sharedWorkspace openURL:target];
}

std::vector<FileApplication> FileApplicationsMac(const base::FilePath& path) {
  @autoreleasepool {
    NSURL* file = base::apple::FilePathToNSURL(path);
    NSWorkspace* workspace = NSWorkspace.sharedWorkspace;
    NSURL* preferred = [workspace URLForApplicationToOpenURL:file];
    std::vector<FileApplication> result;
    for (NSURL* app in [workspace URLsForApplicationsToOpenURL:file]) {
      NSString* name = [NSFileManager.defaultManager displayNameAtPath:app.path];
      result.push_back({base::apple::NSURLToFilePath(app).value(),
                        base::SysNSStringToUTF8(name),
                        [app isEqual:preferred],
                        LoadApplicationIconMac(base::apple::NSURLToFilePath(app), 18)});
    }
    return result;
  }
}

bool IsFilePackageMac(const base::FilePath& path) {
  @autoreleasepool {
    return [NSWorkspace.sharedWorkspace
        isFilePackageAtPath:base::SysUTF8ToNSString(path.value())];
  }
}

std::vector<base::FilePath> ReadFileClipboardMac() {
  @autoreleasepool {
    std::vector<base::FilePath> paths;
    NSArray* urls = [NSPasteboard.generalPasteboard
        readObjectsForClasses:@[NSURL.class]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* url in urls) {
      auto path = base::apple::NSURLToFilePath(url);
      if (path.IsAbsolute() && !path.ReferencesParent()) {
        RememberFileAccess(path);
        paths.push_back(std::move(path));
      }
    }
    return paths;
  }
}

bool HasFileClipboardMac() {
  @autoreleasepool {
    return [NSPasteboard.generalPasteboard
        canReadObjectForClasses:@[NSURL.class]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  }
}

base::FilePath MediaDirectoryMac(bool pictures) {
  @autoreleasepool {
    NSURL* url = [NSFileManager.defaultManager
        URLsForDirectory:(pictures ? NSPicturesDirectory : NSMoviesDirectory)
        inDomains:NSUserDomainMask].firstObject;
    return url ? base::apple::NSURLToFilePath(url) : base::FilePath();
  }
}

SkBitmap DecodeVideoThumbnailMac(const base::FilePath& path, int edge) {
  @autoreleasepool {
    if (!path.IsAbsolute() || edge <= 0 || edge > 128) return {};
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:base::apple::FilePathToNSURL(path)
                                         options:nil];
    AVAssetImageGenerator* generator =
        [[AVAssetImageGenerator alloc] initWithAsset:asset];
    generator.appliesPreferredTrackTransform = YES;
    generator.maximumSize = CGSizeMake(edge, edge);
    struct Result {
      base::Lock lock;
      SkBitmap bitmap;
    };
    auto result = std::make_shared<Result>();
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [generator generateCGImageAsynchronouslyForTime:kCMTimeZero
        completionHandler:^(CGImageRef image, CMTime, NSError*) {
          if (image && CGImageGetWidth(image) <= static_cast<size_t>(edge) &&
              CGImageGetHeight(image) <= static_cast<size_t>(edge)) {
            base::AutoLock guard(result->lock);
            result->bitmap = skia::CGImageToSkBitmap(image);
          }
          dispatch_semaphore_signal(done);
        }];
    // The parent additionally kills this helper on timeout/cancellation.
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,
                                                   6 * NSEC_PER_SEC)) != 0) {
      [generator cancelAllCGImageGeneration];
      return {};
    }
    base::AutoLock guard(result->lock);
    return result->bitmap;
  }
}

}  // namespace crfiles
