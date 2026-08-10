#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int BarnabyChooseLoopbackPort(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 8080;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  int port = 8080;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
    socklen_t length = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) == 0) {
      port = ntohs(addr.sin_port);
    }
  }
  close(fd);
  return port;
}

@interface BarnabyAppDelegate : NSObject <NSApplicationDelegate, WKNavigationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSWindow* splashWindow;
@property(nonatomic, strong) WKWebView* webView;
@property(nonatomic, strong) NSTask* serverTask;
@property(nonatomic, copy) NSString* serverURL;
@end

@implementation BarnabyAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;
  [self configureMainMenu];
  [self configureAppIcon];
  [self showSplashScreen];
  [self startServer];
  [self buildWindow];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
  (void)notification;
  [self.serverTask terminate];
}

- (void)configureMainMenu {
  NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@""];

  NSMenuItem* appMenuItem = [[NSMenuItem alloc] initWithTitle:@""
                                                       action:nil
                                                keyEquivalent:@""];
  [mainMenu addItem:appMenuItem];
  NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"Barnaby"];
  [appMenu addItemWithTitle:@"Quit Barnaby"
                     action:@selector(terminate:)
              keyEquivalent:@"q"];
  appMenuItem.submenu = appMenu;

  NSMenuItem* editMenuItem = [[NSMenuItem alloc] initWithTitle:@""
                                                        action:nil
                                                 keyEquivalent:@""];
  [mainMenu addItem:editMenuItem];
  NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
  [editMenu addItemWithTitle:@"Undo"
                      action:NSSelectorFromString(@"undo:")
               keyEquivalent:@"z"];
  NSMenuItem* redo = [editMenu addItemWithTitle:@"Redo"
                                         action:NSSelectorFromString(@"redo:")
                                  keyEquivalent:@"Z"];
  redo.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
  [editMenu addItem:[NSMenuItem separatorItem]];
  [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [editMenu addItemWithTitle:@"Select All"
                      action:@selector(selectAll:)
               keyEquivalent:@"a"];
  editMenuItem.submenu = editMenu;

  NSApp.mainMenu = mainMenu;
}

- (void)configureAppIcon {
  NSString* iconPath = [[NSBundle mainBundle] pathForResource:@"AppIcon" ofType:@"icns"];
  if (!iconPath) {
    return;
  }

  NSImage* icon = [[NSImage alloc] initWithContentsOfFile:iconPath];
  if (icon) {
    [NSApp setApplicationIconImage:icon];
  }
}

- (NSImage*)barnabyIcon {
  NSImage* icon = NSApp.applicationIconImage;
  if (icon && icon.isValid) {
    return icon;
  }

  NSString* iconPath = [[NSBundle mainBundle] pathForResource:@"AppIcon" ofType:@"icns"];
  if (!iconPath) {
    return nil;
  }
  return [[NSImage alloc] initWithContentsOfFile:iconPath];
}

- (void)showSplashScreen {
  NSRect frame = NSMakeRect(0, 0, 360, 280);
  self.splashWindow = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:NSWindowStyleMaskBorderless
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
  self.splashWindow.title = @"Barnaby";
  self.splashWindow.backgroundColor = [NSColor windowBackgroundColor];
  self.splashWindow.opaque = YES;
  self.splashWindow.releasedWhenClosed = NO;
  self.splashWindow.level = NSFloatingWindowLevel;
  self.splashWindow.hasShadow = YES;

  NSView* content = [[NSView alloc] initWithFrame:frame];
  content.wantsLayer = YES;
  content.layer.cornerRadius = 18.0;
  content.layer.masksToBounds = YES;
  content.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;

  NSImageView* iconView = [[NSImageView alloc] initWithFrame:NSMakeRect(118, 104, 124, 124)];
  iconView.image = [self barnabyIcon];
  iconView.imageScaling = NSImageScaleProportionallyUpOrDown;
  [content addSubview:iconView];

  NSTextField* title = [NSTextField labelWithString:@"Barnaby"];
  title.frame = NSMakeRect(40, 58, 280, 36);
  title.alignment = NSTextAlignmentCenter;
  title.font = [NSFont systemFontOfSize:26 weight:NSFontWeightSemibold];
  title.textColor = [NSColor labelColor];
  [content addSubview:title];

  self.splashWindow.contentView = content;
  [self.splashWindow center];
  [self.splashWindow makeKeyAndOrderFront:nil];
  [self.splashWindow display];
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)dismissSplashScreen {
  [self.splashWindow orderOut:nil];
  [self.splashWindow close];
  self.splashWindow = nil;
}

- (void)startServer {
  NSBundle* bundle = [NSBundle mainBundle];
  NSString* serverPath = [bundle pathForResource:@"gitboard_server_universal" ofType:nil];
  NSString* gitboardPath = [bundle pathForResource:@"gitboard_universal" ofType:nil];
  if (!serverPath || !gitboardPath) {
    [self showFatalError:@"Barnaby.app is missing its bundled command-line helpers."];
    return;
  }

  int port = BarnabyChooseLoopbackPort();
  NSString* apiToken = [[[NSUUID UUID] UUIDString]
      stringByReplacingOccurrencesOfString:@"-" withString:@""];
  self.serverURL =
      [NSString stringWithFormat:@"http://127.0.0.1:%d/?token=%@", port, apiToken];

  NSTask* task = [[NSTask alloc] init];
  task.executableURL = [NSURL fileURLWithPath:serverPath];
  task.arguments = @[
    @"--port",
    [NSString stringWithFormat:@"%d", port],
    @"--gitboard-path",
    gitboardPath,
    @"--api-token",
    apiToken,
    @"--no-open-browser"
  ];
  task.standardOutput = [NSPipe pipe];
  task.standardError = [NSPipe pipe];

  NSError* error = nil;
  if (![task launchAndReturnError:&error]) {
    [self showFatalError:[NSString stringWithFormat:@"Could not start Barnaby server: %@",
                                                    error.localizedDescription]];
    return;
  }
  self.serverTask = task;
}

- (void)buildWindow {
  NSRect frame = NSMakeRect(0, 0, 1180, 760);
  self.window = [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:NSWindowStyleMaskTitled |
                                                      NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable |
                                                      NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  self.window.title = @"Barnaby";
  [self.window center];

  WKWebViewConfiguration* configuration = [[WKWebViewConfiguration alloc] init];
  self.webView = [[WKWebView alloc] initWithFrame:self.window.contentView.bounds
                                    configuration:configuration];
  self.webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  self.webView.navigationDelegate = self;
  self.window.contentView = self.webView;

  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  [self dismissSplashScreen];

  if (self.serverURL) {
    [self loadServerAfterDelay:0.15];
  }
}

- (void)loadServerAfterDelay:(NSTimeInterval)delay {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                               static_cast<int64_t>(delay * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   NSURL* url = [NSURL URLWithString:self.serverURL];
                   [self.webView loadRequest:[NSURLRequest requestWithURL:url]];
                 });
}

- (void)webView:(WKWebView*)webView
  didFailProvisionalNavigation:(WKNavigation*)navigation
                     withError:(NSError*)error {
  (void)webView;
  (void)navigation;
  if (self.serverTask.isRunning) {
    [self loadServerAfterDelay:0.25];
    return;
  }
  [self showFatalError:[NSString stringWithFormat:@"Barnaby server stopped before the UI loaded: %@",
                                                  error.localizedDescription]];
}

- (void)showFatalError:(NSString*)message {
  [self dismissSplashScreen];
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"Barnaby could not start";
  alert.informativeText = message;
  [alert addButtonWithTitle:@"Quit"];
  [alert runModal];
  [NSApp terminate:nil];
}

@end

int main(int argc, const char* argv[]) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    NSApplication* app = [NSApplication sharedApplication];
    BarnabyAppDelegate* delegate = [[BarnabyAppDelegate alloc] init];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
