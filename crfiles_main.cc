// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <clocale>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/at_exit.h"
#include "base/base_paths.h"
#include "base/base_switches.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/debug/stack_trace.h"
#include "base/feature_list.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/icu_util.h"
#include "base/i18n/rtl.h"
#include "base/logging.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/path_service.h"
#include "base/power_monitor/power_monitor.h"
#include "base/power_monitor/power_monitor_device_source.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/discardable_memory/service/discardable_shared_memory_manager.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_version.h"
#include "crFiles/crfiles_window.h"
#include "libcr/viz_context_factory.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/platform/ax_platform.h"
#include "ui/base/ime/init/input_method_initializer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/resource/resource_scale_factor.h"
#include "ui/base/ui_base_paths.h"
#include "ui/display/screen.h"
#include "ui/gfx/font_util.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/init/gl_factory.h"
#include "ui/views/buildflags.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/views_delegate.h"
#include "ui/views/widget/widget.h"

#if BUILDFLAG(ENABLE_DESKTOP_AURA)
#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"
#include "ui/views/widget/desktop_aura/desktop_screen.h"
#include "ui/views/widget/native_widget_aura.h"
#endif

#if BUILDFLAG(IS_LINUX)
#include "ui/linux/display_server_utils.h"
#include "ui/linux/linux_ui.h"
#include "ui/linux/linux_ui_delegate_stub.h"
#include "ui/linux/linux_ui_factory.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include "ui/base/win/scoped_ole_initializer.h"
#endif

#if defined(USE_AURA)
#include "ui/aura/env.h"
#include "ui/wm/core/wm_state.h"
#endif

#if BUILDFLAG(IS_OZONE)
#include "ui/ozone/public/gpu_platform_support_host.h"
#include "ui/ozone/public/ozone_platform.h"
#endif

namespace {

constexpr char kDefaultLocale[] = "en-US";

bool IsSupportedLocale(std::string_view locale) {
  constexpr std::string_view kLocales[] = {
      "en-US", "zh-CN", "zh-HK", "zh-TW", "ar", "fr",
      "fr-CA", "ru", "es", "es-419", "pt-BR", "pt-PT"};
  for (std::string_view candidate : kLocales) {
    if (locale == candidate) {
      return true;
    }
  }
  return false;
}

std::string ResolveLocale(std::string_view locale) {
  const std::string canonical = base::i18n::GetCanonicalLocale(locale);
  if (IsSupportedLocale(canonical)) {
    return canonical;
  }
  const std::string_view language = l10n_util::GetLanguage(canonical);
  if (language == "zh") {
    if (canonical.find("HK") != std::string::npos) {
      return "zh-HK";
    }
    if (canonical.find("TW") != std::string::npos ||
        canonical.find("Hant") != std::string::npos) {
      return "zh-TW";
    }
    return "zh-CN";
  }
  if (language == "fr") {
    return canonical.find("CA") != std::string::npos ? "fr-CA" : "fr";
  }
  if (language == "es") {
    return canonical.find("ES") != std::string::npos ? "es" : "es-419";
  }
  if (language == "pt") {
    return canonical.find("PT") != std::string::npos ? "pt-PT" : "pt-BR";
  }
  if (language == "ar" || language == "ru") {
    return std::string(language);
  }
  return kDefaultLocale;
}

void InitializeResources(const std::string& locale) {
  base::FilePath assets;
  CHECK(base::PathService::Get(base::DIR_ASSETS, &assets));
  const base::FilePath resources =
      assets.AppendASCII("crfiles_resources.pak");
  const base::FilePath locale_pack =
      assets.AppendASCII("crfiles_resources_" + locale + ".pak");
  ui::ResourceBundle::InitSharedInstanceWithPakFileRegion(
      base::File(base::PathExists(locale_pack) ? locale_pack : resources,
                 base::File::FLAG_OPEN | base::File::FLAG_READ),
      base::MemoryMappedFile::Region::kWholeFile);
  ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
      resources, ui::k100Percent);
  const base::FilePath resources_200 =
      assets.AppendASCII("crfiles_resources_200_percent.pak");
  if (base::PathExists(resources_200)) {
    ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
        resources_200, ui::k200Percent);
  }
}

class CrFilesViewsDelegate : public views::ViewsDelegate {
 public:
  void OnBeforeWidgetInit(
      views::Widget::InitParams* params,
      views::internal::NativeWidgetDelegate* delegate) override {
#if BUILDFLAG(ENABLE_DESKTOP_AURA)
    if (params->native_widget) {
      return;
    }
    if (params->parent &&
        params->type != views::Widget::InitParams::TYPE_MENU &&
        params->type != views::Widget::InitParams::TYPE_TOOLTIP) {
      params->native_widget = new views::NativeWidgetAura(delegate);
    } else {
      params->native_widget = new views::DesktopNativeWidgetAura(delegate);
    }
#endif
  }
  std::string GetApplicationName() override { return "crFiles"; }
};

class CrFilesAXPlatformDelegate : public ui::AXPlatform::Delegate {
 public:
  ui::AXMode GetAccessibilityMode() override { return ui::AXMode(); }
#if BUILDFLAG(IS_WIN)
  ui::AXPlatform::ProductStrings GetProductStrings() override {
    return {.product_name = "crFiles",
            .product_version = CRFILES_VERSION_STRING,
            .toolkit_version = "Chromium Views"};
  }
#endif
};

#if BUILDFLAG(IS_OZONE)
class CrFilesOzoneGpuConnection {
 public:
  void Initialize() {
    ui::OzonePlatform::GetInstance()->AddInterfaces(&binders_);
    ui::OzonePlatform::GetInstance()->GetGpuPlatformSupportHost()
        ->OnGpuServiceLaunched(
            1,
            base::BindRepeating(&CrFilesOzoneGpuConnection::BindInterface,
                                base::Unretained(this)),
            base::DoNothing());
  }

 private:
  void BindInterface(const std::string& interface_name,
                     mojo::ScopedMessagePipeHandle pipe) {
    mojo::GenericPendingReceiver receiver(interface_name, std::move(pipe));
    CHECK(binders_.TryBind(&receiver));
  }

  mojo::BinderMap binders_;
};
#endif

int RunApplication() {
#if BUILDFLAG(IS_WIN)
  ui::ScopedOleInitializer ole_initializer;
#endif
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  base::debug::EnableInProcessStackDumping();
  command_line->AppendSwitch("single-process");
  std::string enabled_features =
      command_line->GetSwitchValueASCII(switches::kEnableFeatures);
  base::FeatureList::InitInstance(
      enabled_features,
      command_line->GetSwitchValueASCII(switches::kDisableFeatures));
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::UI);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFiles");
  base::ScopedClosureRunner thread_pool_shutdown(base::BindOnce([] {
    base::ThreadPoolInstance::Get()->Shutdown();
  }));
  auto discardable_memory_manager =
      std::make_unique<discardable_memory::DiscardableSharedMemoryManager>();
  base::DiscardableMemoryAllocator::SetInstance(
      discardable_memory_manager.get());
  base::ScopedClosureRunner reset_discardable_memory_allocator(
      base::BindOnce([] {
        base::DiscardableMemoryAllocator::SetInstance(nullptr);
      }));
  base::Thread mojo_thread("crFilesMojoThread");
  CHECK(mojo_thread.StartWithOptions(
      base::Thread::Options(base::MessagePumpType::IO, 0)));
  mojo::core::ScopedIPCSupport ipc_support(
      mojo_thread.task_runner(),
      mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);

#if BUILDFLAG(IS_OZONE)
#if BUILDFLAG(IS_LINUX)
  ui::SetOzonePlatformForLinuxIfNeeded(*command_line);
#endif
  ui::OzonePlatform::PreSandboxStartup();
  ui::OzonePlatform::InitParams ozone_params;
  ozone_params.single_process = true;
  ui::OzonePlatform::InitializeForUI(ozone_params);
  ui::OzonePlatform::InitializeForGPU(ozone_params);
  ui::OzonePlatform::GetInstance()->PostCreateMainMessageLoop(
      base::DoNothing(), base::SingleThreadTaskRunner::GetCurrentDefault());
  CrFilesOzoneGpuConnection ozone_gpu_connection;
  ozone_gpu_connection.Initialize();
#endif
#if !BUILDFLAG(IS_WIN)
  gl::init::InitializeGLOneOff(gl::GpuPreference::kDefault);
#endif
#if BUILDFLAG(IS_LINUX)
  std::unique_ptr<ui::LinuxUiDelegateStub> fallback_delegate;
  if (!ui::LinuxUiDelegate::GetInstance()) {
    fallback_delegate = std::make_unique<ui::LinuxUiDelegateStub>();
  }
  ui::LinuxUi* previous_linux_ui =
      ui::LinuxUi::SetInstance(ui::GetDefaultLinuxUi());
#endif
  gfx::InitializeFonts();
  ui::InitializeInputMethod();
  base::PowerMonitor::GetInstance()->Initialize(
      std::make_unique<base::PowerMonitorDeviceSource>());
#if defined(USE_AURA)
  std::unique_ptr<aura::Env> env = aura::Env::CreateInstance();
  libcr::VizContextFactory context_factory;
  env->set_context_factory(&context_factory);
  wm::WMState wm_state;
#endif
#if BUILDFLAG(ENABLE_DESKTOP_AURA)
  std::unique_ptr<display::Screen> screen = views::CreateDesktopScreen();
#endif
  views::LayoutProvider layout_provider;
  CrFilesAXPlatformDelegate ax_delegate;
  ui::AXPlatform ax_platform(ax_delegate);
  CrFilesViewsDelegate views_delegate;
  base::RunLoop run_loop(base::RunLoop::Type::kNestableTasksAllowed);
  crfiles::ShowCrFilesWindow(run_loop.QuitClosure());
  run_loop.Run();
  crfiles::DestroyCrFilesWindow();
#if BUILDFLAG(IS_LINUX)
  ui::LinuxUi::SetInstance(previous_linux_ui);
#endif
  ui::ShutdownInputMethod();
#if defined(USE_AURA)
  env.reset();
#endif
  return 0;
}

int CrFilesEntry(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");
#endif
  base::AtExitManager at_exit;
  mojo::core::Configuration mojo_configuration;
  mojo_configuration.is_broker_process = true;
  mojo::core::Init(mojo_configuration);
  base::i18n::InitializeICU();
  ui::RegisterPathProvider();
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  std::string configured_locale;
  if (command_line.HasSwitch("lang")) {
    configured_locale = command_line.GetSwitchValueASCII("lang");
  } else {
    configured_locale = crfiles::GetCrFilesLanguageSetting();
    if (configured_locale.empty()) {
      configured_locale = base::i18n::GetConfiguredLocale();
    }
  }
  const std::string locale = ResolveLocale(configured_locale);
  base::i18n::SetICUDefaultLocale(locale);
  InitializeResources(locale);
  const int result = RunApplication();
  ui::ResourceBundle::CleanupSharedInstance();
  return result;
}

}  // namespace

#if BUILDFLAG(IS_WIN)
int APIENTRY WinMain(HINSTANCE, HINSTANCE, char*, int) {
  return CrFilesEntry(0, nullptr);
}
#else
int main(int argc, char** argv) {
  return CrFilesEntry(argc, argv);
}
#endif
