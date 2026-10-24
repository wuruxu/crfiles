// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crFiles/crfiles_file_access.h"
#include "crFiles/crfiles_process_singleton.h"
#include <clocale>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/allocator/partition_alloc_support.h"
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
#include "base/memory_coordinator/dummy_memory_consumer_registry.h"
#include "base/memory_coordinator/memory_consumer_registry.h"
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
#if BUILDFLAG(IS_WIN)
#include "crQuickShare/bluetooth_peripheral_win.h"
#include <roapi.h>
#endif
#if BUILDFLAG(IS_LINUX)
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/dbus/dbus_bluez_manager_wrapper_linux.h"
#endif
#include "components/discardable_memory/service/discardable_shared_memory_manager.h"
#include "crFiles/crfiles_theme.h"
#include "crFiles/crfiles_search.h"
#include "crFiles/crfiles_archive.h"
#include "crFiles/crfiles_video_thumbnail.h"
#include "crFiles/crfiles_version.h"
#include "crFiles/crfiles_window.h"
#include "libcr/viz_context_factory.h"
#include "libcr/startup_timing.h"
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

#if BUILDFLAG(IS_MAC)
#include "crFiles/crfiles_mac.h"
#include "crQuickShare/mac_sdp_client.h"
#endif

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
base::TimeTicks startup_last_stage_at;

void LogStartupStage(const char* stage) {
  startup_last_stage_at =
      libcr::LogStartupTiming(stage, startup_last_stage_at);
}

bool IsSupportedLocale(std::string_view locale) {
  constexpr std::string_view kLocales[] = {
      "en-US", "ja", "ko", "zh-CN", "zh-HK", "zh-TW", "ar", "fr",
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
  const std::string_view language = std::string_view(canonical).substr(0, canonical.find('-'));
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
  if (language == "ar" || language == "ru" ||
      language == "ja" || language == "ko") {
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
#if BUILDFLAG(IS_MAC)
    if (!params->native_widget && params->name == "crfiles_window") {
      params->native_widget = crfiles::CreateCrFilesNativeWidgetMac(delegate);
    }
#elif BUILDFLAG(ENABLE_DESKTOP_AURA)
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
#if BUILDFLAG(IS_MAC)
  ui::ContextFactory* GetContextFactory() override {
    return libcr::VizContextFactory::Get();
  }
#endif
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

// IPC can arrive while startup is pumping native UI events. Defer activation
// until the main window is fully initialized, and ignore it during teardown.
struct SingletonActivation {
  bool ready = false;
  bool pending = false;
  bool shutting_down = false;
  void Request() {
    if (shutting_down) return;
    if (!ready) { pending = true; return; }
    crfiles::RestoreCrFilesWindow();
  }
};

int RunApplication() {
#if BUILDFLAG(IS_MAC)
  crfiles::InitializeCrFilesApplicationMac();
#endif
#if BUILDFLAG(IS_WIN)
  ui::ScopedOleInitializer ole_initializer;
  // BLE/GATT WinRT calls share the existing STA UI apartment.
  const HRESULT winrt_result = RoInitialize(RO_INIT_SINGLETHREADED);
  base::ScopedClosureRunner winrt_cleanup(base::BindOnce([](HRESULT result) {
    if (SUCCEEDED(result)) {
      LOG(INFO) << "crFiles shutdown: stage=winrt; event=started";
      RoUninitialize();
      LOG(INFO) << "crFiles shutdown: stage=winrt; event=finished";
    }
  }, winrt_result));
  if (FAILED(winrt_result)) LOG(WARNING) << "Windows Runtime initialization failed; hr=" << winrt_result;
#endif
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  base::debug::EnableInProcessStackDumping();
  LogStartupStage("ole_stack_dump_ready");
  command_line->AppendSwitch("single-process");
  // This standalone UI process does not run ContentMain's allocator setup.
  auto* partition_alloc = base::allocator::PartitionAllocSupport::Get();
  const std::string process_type = command_line->GetSwitchValueASCII("type");
  partition_alloc->ReconfigureEarlyish(process_type);
  std::string enabled_features =
      command_line->GetSwitchValueASCII(switches::kEnableFeatures);
  base::FeatureList::InitInstance(
      enabled_features,
      command_line->GetSwitchValueASCII(switches::kDisableFeatures));
  // Enable partition thread caches before starting the Viz compositor thread.
  partition_alloc->ReconfigureAfterFeatureListInit(process_type);
  base::SingleThreadTaskExecutor executor(base::MessagePumpType::UI);
  LogStartupStage("main_scheduler_ready");
#if BUILDFLAG(IS_MAC)
  if (command_line->HasSwitch(crquickshare::kMacSdpProbeSwitch)) {
    // Isolate one SDP query before singleton activation, windows or discovery.
    base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFilesSdpProbe");
    partition_alloc->ReconfigureAfterTaskRunnerInit(process_type);
    const int result = crquickshare::RunMacSdpProbe(
        command_line->GetSwitchValueASCII(crquickshare::kMacSdpProbeSwitch));
    base::ThreadPoolInstance::Get()->Shutdown();
    return result;
  }
#endif
  auto singleton_activation = std::make_shared<SingletonActivation>();
  crfiles::CrFilesProcessSingleton process_singleton(base::BindRepeating(
      [](std::shared_ptr<SingletonActivation> activation) { activation->Request(); },
      singleton_activation));
  const auto singleton_result = process_singleton.NotifyOtherProcessOrCreate();
  if (singleton_result == crfiles::CrFilesProcessSingleton::Result::kNotifiedExistingInstance) {
    LOG(INFO) << "crFiles singleton: existing instance notified";
    return 0;
  }
  if (singleton_result == crfiles::CrFilesProcessSingleton::Result::kError) return 1;
  LOG(INFO) << "crFiles singleton: primary instance";
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("crFiles");
  partition_alloc->ReconfigureAfterTaskRunnerInit(process_type);
  base::ScopedClosureRunner thread_pool_shutdown(base::BindOnce([] {
    const auto started = base::TimeTicks::Now();
    LOG(INFO) << "crFiles shutdown: stage=thread_pool; event=started";
    base::ThreadPoolInstance::Get()->Shutdown();
    LOG(INFO) << "crFiles shutdown: stage=thread_pool; event=finished; elapsed_ms="
              << (base::TimeTicks::Now() - started).InMilliseconds();
  }));
#if BUILDFLAG(IS_LINUX)
  bluez::DBusBluezManagerWrapperLinux::Initialize();
  base::ScopedClosureRunner bluetooth_shutdown(base::BindOnce([] {
    device::BluetoothAdapterFactory::Shutdown();
    bluez::DBusBluezManagerWrapperLinux::Shutdown();
  }));
#endif
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
  LogStartupStage("mojo_ipc_ready");

#if BUILDFLAG(IS_WIN)
  // Start GPU initialization while the UI thread initializes fonts and other
  // platform services. Keep Viz alive until after Aura has been destroyed.
  libcr::VizContextFactory context_factory;
  LogStartupStage("viz_start_requested");
#endif

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
#if !BUILDFLAG(IS_WIN) && !BUILDFLAG(IS_MAC)
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
  LogStartupStage("platform_gl_ready");
  gfx::InitializeFonts();
  LogStartupStage("font_system_ready");
  ui::InitializeInputMethod();
  LogStartupStage("input_method_ready");
  base::PowerMonitor::GetInstance()->Initialize(
      std::make_unique<base::PowerMonitorDeviceSource>());
  LogStartupStage("power_monitor_ready");
#if defined(USE_AURA)
  std::unique_ptr<aura::Env> env = aura::Env::CreateInstance();
#endif
  // Both Aura and Cocoa windows need a compositor context factory.
#if !BUILDFLAG(IS_WIN)
  libcr::VizContextFactory context_factory;
#endif
#if defined(USE_AURA)
  env->set_context_factory(&context_factory);
  wm::WMState wm_state;
#endif
#if BUILDFLAG(IS_MAC)
  // Cocoa window bridges require a registered Screen when adding observers.
  display::ScopedNativeScreen screen;
#elif BUILDFLAG(ENABLE_DESKTOP_AURA)
  std::unique_ptr<display::Screen> screen = views::CreateDesktopScreen();
#endif
  LogStartupStage("viz_screen_ready");
  views::LayoutProvider layout_provider;
  CrFilesAXPlatformDelegate ax_delegate;
  ui::AXPlatform ax_platform(ax_delegate);
  CrFilesViewsDelegate views_delegate;
  base::RunLoop run_loop(base::RunLoop::Type::kNestableTasksAllowed);
  crfiles::ShowCrFilesWindow(run_loop.QuitClosure());
  singleton_activation->ready = true;
  if (singleton_activation->pending) singleton_activation->Request();
  LogStartupStage("window_shown");
  run_loop.Run();
  singleton_activation->shutting_down = true;
  LOG(INFO) << "crFiles shutdown: stage=main_loop; event=exited";
  const auto window_destroy_started = base::TimeTicks::Now();
  LOG(INFO) << "crFiles shutdown: stage=destroy_window; event=started";
  crfiles::DestroyCrFilesWindow();
  LOG(INFO) << "crFiles shutdown: stage=destroy_window; event=finished; elapsed_ms="
            << (base::TimeTicks::Now() - window_destroy_started).InMilliseconds();
#if BUILDFLAG(IS_WIN)
  crquickshare::BluetoothPeripheralWin::DrainPendingStops();
#endif
  // Native close notifications defer client-owned bubble destruction. Finish
  // those UI tasks while the Views delegate and Viz service are still alive.
  LOG(INFO) << "crFiles shutdown: stage=drain_ui_tasks; event=started";
  base::RunLoop().RunUntilIdle();
  LOG(INFO) << "crFiles shutdown: stage=drain_ui_tasks; event=finished";
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
  libcr::InitializeStartupTiming("crFiles");
  startup_last_stage_at = base::TimeTicks::Now();
  base::CommandLine::Init(argc, argv);
#if BUILDFLAG(IS_WIN)
  logging::SetLogItems(true, true, true, false);
#endif
#if BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_ANDROID)
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");
#endif
  base::AtExitManager at_exit;
#if BUILDFLAG(IS_MAC)
  crfiles::InitializeCrFilesBundle();
#endif
  // Standalone processes must register memory consumers before creating them.
  base::ScopedMemoryConsumerRegistry<base::DummyMemoryConsumerRegistry>
      memory_consumer_registry;
  mojo::core::Configuration mojo_configuration;
  mojo_configuration.is_broker_process = true;
  mojo::core::Init(mojo_configuration);
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(crfiles::kVideoThumbnailProcessSwitch)) {
    base::FeatureList::InitInstance("", "");
    return crfiles::RunVideoThumbnailProcess();
  }
#if BUILDFLAG(IS_MAC)
  // The inherited-sandbox executable is a worker, never another UI entry point.
  const auto* launch = base::CommandLine::ForCurrentProcess();
  if (launch->GetProgram().BaseName().value() == "crFiles Helper" &&
      !launch->HasSwitch(crfiles::kArchiveProcessSwitch) &&
      !launch->HasSwitch(crfiles::kSearchProcessSwitch)) return 1;
#endif
  base::i18n::InitializeICU();
  LogStartupStage("mojo_icu_ready");
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(crfiles::kArchiveProcessSwitch)) {
    base::FeatureList::InitInstance("", "");
    return crfiles::RunArchiveProcess();
  }
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(crfiles::kSearchProcessSwitch)) {
    base::FeatureList::InitInstance("", "");
    return crfiles::RunSearchProcess();
  }
  ui::RegisterPathProvider();
#if BUILDFLAG(IS_MAC)
  crfiles::RestoreFileAccess();
#endif
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
  LogStartupStage("resources_ready");
  const int result = RunApplication();
  LOG(INFO) << "crFiles shutdown: stage=application; event=returned";
  ui::ResourceBundle::CleanupSharedInstance();
  return result;
}

}  // namespace

#if BUILDFLAG(IS_WIN)
int APIENTRY WinMain(HINSTANCE, HINSTANCE, char*, int) {
  const int result = CrFilesEntry(0, nullptr);
  LOG(INFO) << "crFiles shutdown: stage=winmain; event=returning; exit_code=" << result;
  return result;
}
#else
int main(int argc, char** argv) {
  return CrFilesEntry(argc, argv);
}
#endif
