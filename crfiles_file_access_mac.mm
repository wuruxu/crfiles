// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license in LICENSE.
#include "crFiles/crfiles_file_access.h"
#include "crFiles/grit/crfiles_strings.h"
#import <AppKit/AppKit.h>
#include <Security/SecTask.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <memory>
#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/sys_string_conversions.h"
#include "base/check.h"
#include "base/synchronization/lock.h"
#include "base/task/thread_pool.h"
#include "ui/views/widget/widget.h"
#include "ui/base/l10n/l10n_util_mac.h"

namespace crfiles {
namespace {
base::FilePath UserDirectory(NSSearchPathDirectory kind) {
  NSArray<NSURL*>* urls = [NSFileManager.defaultManager URLsForDirectory:kind
                                                             inDomains:NSUserDomainMask];
  return urls.count ? base::apple::NSURLToFilePath(urls.firstObject) : base::FilePath();
}
base::FilePath BundleRoot() {
  base::FilePath executable;
  if (!base::PathService::Get(base::FILE_EXE, &executable)) return {};
  const auto contents = executable.DirName().DirName();
  if (contents.BaseName().value() != "Contents" ||
      !base::PathExists(contents.AppendASCII("Info.plist"))) return {};
  return contents.DirName();
}
struct Grants {
  base::Lock lock;
  NSMutableDictionary<NSString*, NSData*>* __strong bookmarks =
      [[NSMutableDictionary alloc] init];
  // Keep grants alive for background sharing, independently of open tabs.
  NSMutableDictionary<NSString*, NSURL*>* __strong urls =
      [[NSMutableDictionary alloc] init];
};
Grants& Access() { static base::NoDestructor<Grants> grants; return *grants; }
NSURL* BookmarkFile() {
  return base::apple::FilePathToNSURL(
      MacApplicationSupportDirectory().AppendASCII("file-access.plist"));
}
bool Activate(Grants& grants, NSData* data) {
  BOOL stale = NO;
  NSURL* url = [NSURL URLByResolvingBookmarkData:data
      options:NSURLBookmarkResolutionWithSecurityScope | NSURLBookmarkResolutionWithoutUI
      relativeToURL:nil bookmarkDataIsStale:&stale error:nil];
  if (!url) return false;
  if (grants.urls[url.path]) return true;
  if (![url startAccessingSecurityScopedResource]) return false;
  grants.urls[url.path] = url;
  NSData* refreshed = stale ? [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
      includingResourceValuesForKeys:nil relativeToURL:nil error:nil] : data;
  grants.bookmarks[url.path] = refreshed ?: data;
  return true;
}
void SaveGrants(const Grants& grants) {
  const auto directory = MacApplicationSupportDirectory();
  if (!base::CreateDirectory(directory) || !base::SetPosixFilePermissions(directory, 0700)) return;
  [grants.bookmarks writeToURL:BookmarkFile() atomically:YES];
  base::SetPosixFilePermissions(base::apple::NSURLToFilePath(BookmarkFile()), 0600);
}
base::FilePath ExistingParent(base::FilePath path) {
  while (!path.empty()) {
    struct stat info;
    if (stat(path.value().c_str(), &info) == 0) return path;
    // An inaccessible ancestor is not a missing directory. Falling back to /
    // here would incorrectly mark a sandbox-denied destination as accessible.
    if (errno != ENOENT) return path;
    const auto parent = path.DirName();
    if (parent == path) return {};
    path = parent;
  }
  return {};
}
bool ContainsPath(const base::FilePath& parent, const base::FilePath& path) {
  return !parent.empty() && (parent == path || parent.IsParent(path));
}
bool Accessible(const base::FilePath& path, bool write) {
  const auto existing = write ? ExistingParent(path) : path;
  if (existing.empty()) return false;
  // access() alone is not proof that a sandbox permits reading the directory.
  base::ScopedFD directory(HANDLE_EINTR(open(existing.value().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)));
  if (!directory.is_valid()) return false;
  if (!write) return true;
  if (access(existing.value().c_str(), W_OK | X_OK) != 0) return false;
  if (ContainsPath(base::GetHomeDir(), existing) ||
      ContainsPath(MacDownloadsDirectory(), existing)) return true;
  auto& grants = Access();
  base::AutoLock lock(grants.lock);
  for (NSString* key in grants.urls) {
    if (ContainsPath(base::apple::NSStringToFilePath(key), existing)) return true;
  }
  return false;
}
void AccessChecked(base::WeakPtr<views::Widget> widget, base::FilePath path,
                   bool write, base::OnceCallback<void(bool)> reply, bool allowed) {
  if (allowed) { std::move(reply).Run(true); return; }
  if (!widget || widget->IsClosed()) { std::move(reply).Run(false); return; }
  NSWindow* window = widget->GetNativeWindow().GetNativeNSWindow();
  // Never pile up sheets when several restored panes request access together.
  if (!window || window.attachedSheet) { std::move(reply).Run(false); return; }
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = NO;
  panel.canChooseDirectories = YES;
  panel.allowsMultipleSelection = NO;
  panel.directoryURL = base::apple::FilePathToNSURL(path);
  panel.message = l10n_util::GetNSString(IDS_CRFILES_FOLDER_ACCESS_PROMPT);
  auto completion = std::make_shared<base::OnceCallback<void(bool)>>(std::move(reply));
  [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
    if (response != NSModalResponseOK || !widget || widget->IsClosed()) {
      std::move(*completion).Run(false); return;
    }
    const auto selected = base::apple::NSURLToFilePath(panel.URL).StripTrailingSeparators();
    const auto requested = path.StripTrailingSeparators();
    if (selected != requested && !selected.IsParent(requested)) {
      std::move(*completion).Run(false); return;
    }
    RememberFileAccess(selected);
    base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
        base::BindOnce(&Accessible, path, write), std::move(*completion));
  }];
}
}  // namespace
bool IsCrFilesSandboxed() {
  static const bool sandboxed = [] {
    base::apple::ScopedCFTypeRef<SecTaskRef> task(SecTaskCreateFromSelf(kCFAllocatorDefault));
    if (!task) return false;
    base::apple::ScopedCFTypeRef<CFTypeRef> value(
        SecTaskCopyValueForEntitlement(task.get(), CFSTR("com.apple.security.app-sandbox"), nullptr));
    return value && CFEqual(value.get(), kCFBooleanTrue);
  }();
  return sandboxed;
}
base::FilePath MacApplicationSupportDirectory() {
  const auto root = UserDirectory(NSApplicationSupportDirectory);
  CHECK(!root.empty());
  return root.AppendASCII("crFiles");
}
base::FilePath MacCacheDirectory() {
  const auto root = UserDirectory(NSCachesDirectory);
  CHECK(!root.empty());
  return root.AppendASCII("crFiles");
}
base::FilePath MacUserHomeDirectory() {
  return base::apple::NSStringToFilePath(NSHomeDirectoryForUser(NSUserName()));
}
base::FilePath MacDownloadsDirectory() { return UserDirectory(NSDownloadsDirectory); }
void InitializeCrFilesBundle() {
  const auto root = BundleRoot();
  if (root.empty()) return;
  base::apple::SetOverrideFrameworkBundlePath(root);
  base::apple::SetOverrideOuterBundlePath(root);
  NSBundle* bundle = [NSBundle bundleWithURL:base::apple::FilePathToNSURL(root)];
  NSString* group = [bundle objectForInfoDictionaryKey:@"CrFilesIPCGroup"];
  CHECK([group isKindOfClass:NSString.class] && group.length);
  base::apple::SetBaseBundleIDOverride(base::SysNSStringToUTF8(group));
  base::PathService::Override(base::DIR_ASSETS,
      root.AppendASCII("Contents").AppendASCII("Resources"));
}
bool GetCrFilesHelperExecutable(base::FilePath* executable) {
  const auto root = BundleRoot();
  if (root.empty()) return base::PathService::Get(base::FILE_EXE, executable);
  *executable = root.AppendASCII("Contents").AppendASCII("Helpers").AppendASCII("crFiles Helper");
  return base::PathExists(*executable);
}
void RestoreFileAccess() {
  if (!IsCrFilesSandboxed()) return;
  auto& grants = Access();
  base::AutoLock lock(grants.lock);
  NSDictionary<NSString*, NSData*>* saved = [NSDictionary dictionaryWithContentsOfURL:BookmarkFile() error:nil];
  for (id key in saved) {
    id value = saved[key];
    if (![key isKindOfClass:NSString.class] || ![value isKindOfClass:NSData.class]) continue;
    // An offline volume must not lose its durable grant merely because it
    // cannot be resolved during this launch.
    grants.bookmarks[key] = value;
    Activate(grants, value);
  }
  if (saved) SaveGrants(grants);
}
void RememberFileAccess(const base::FilePath& path) {
  if (!IsCrFilesSandboxed() || path.empty()) return;
  auto& grants = Access();
  base::AutoLock lock(grants.lock);
  NSURL* url = base::apple::FilePathToNSURL(path);
  if (grants.urls[url.path]) return;
  NSData* data = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
      includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
  if (!data || !Activate(grants, data)) return;
  SaveGrants(grants);
}
std::vector<uint8_t> CreateFileAccessBookmark(const base::FilePath& path) {
  if (!IsCrFilesSandboxed()) return {};
  // Implicit security scope is intentional: explicit app-scoped bookmarks
  // cannot transfer a dynamic sandbox grant to another process.
  NSData* data = [base::apple::FilePathToNSURL(path) bookmarkDataWithOptions:0
      includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
  if (!data) return {};
  const auto bytes = base::apple::NSDataToSpan(data);
  return {bytes.begin(), bytes.end()};
}
base::ScopedClosureRunner ResolveFileAccessBookmark(const std::vector<uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > 1024 * 1024) return {};
  NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
  NSURL* url = [NSURL URLByResolvingBookmarkData:data options:NSURLBookmarkResolutionWithoutUI
      relativeToURL:nil bookmarkDataIsStale:nullptr error:nil];
  if (!url) return {};
  // Resolving an implicit bookmark acquires its grant; retain the URL for the
  // whole operation. There is no explicit start/stop pair for implicit scope.
  return base::ScopedClosureRunner(base::BindOnce([](NSURL* retained) {
    (void)retained;
  }, url));
}
void EnsureDirectoryAccess(views::Widget* parent, const base::FilePath& path,
                           bool write, base::OnceCallback<void(bool)> reply) {
  if (!IsCrFilesSandboxed()) { std::move(reply).Run(true); return; }
  if (!path.IsAbsolute() || path.ReferencesParent()) { std::move(reply).Run(false); return; }
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
      base::BindOnce(&Accessible, path, write),
      base::BindOnce(&AccessChecked, parent ? parent->GetWeakPtr() : base::WeakPtr<views::Widget>(),
          path, write, std::move(reply)));
}
}  // namespace crfiles
