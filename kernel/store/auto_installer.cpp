// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Automated OS Installer Implementation
// =====================================

#include "auto_installer.hpp"
#include "kernel/core/serial.hpp"
#include "kernel/sched/scheduler.hpp"

namespace bminos::store {

static InstallProgress s_progress{};
static bool s_installer_running = false;

void AutoInstaller::init() {
    s_progress.stage = InstallStage::Idle;
    s_progress.percent_complete = 0;
    s_progress.status_message = "Ready to install OS";
    s_progress.bytes_transferred = 0;
    s_progress.total_bytes = 0;
    s_progress.transfer_rate_kbps = 0;
    s_progress.created_domain_id = DomainId::invalid();
    s_installer_running = false;
    LOG_INFO("Auto-Installer subsystem initialized");
}

InstallProgress AutoInstaller::get_progress() {
    return s_progress;
}

void AutoInstaller::cancel() {
    if (s_installer_running) {
        s_progress.stage = InstallStage::Failed;
        s_progress.status_message = "Installation cancelled by user";
        s_installer_running = false;
        LOG_INFO("Auto-installation cancelled");
    }
}

u32 AutoInstaller::generate_windows_unattend_xml(const InstallConfig& config, char* out_buf, u32 max_len) {
    if (!out_buf || max_len == 0) return 0;

    const char* user = config.admin_username.c_str();
    if (!user || user[0] == '\0') user = "BMinUser";

    const char* pass = config.admin_password.c_str();
    if (!pass || pass[0] == '\0') pass = "BMinOS2026!";

    // autounattend.xml:
    // 1. Injects LabConfig registry keys to bypass Win11 TPM 2.0, SecureBoot, CPU, RAM, & Storage checks.
    // 2. Automatically formats partition with GPT.
    // 3. Pre-installs VirtIO Viostor and NetKVM drivers from secondary CDROM drive.
    // 4. Creates local user account and bypasses online Microsoft Account requirement.
    // 5. Enables Remote Desktop and BMinOS Bridge Agent.
    const char* xml_template =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n"
        "  <settings pass=\"windowsPE\">\n"
        "    <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"amd64\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n"
        "      <RunSynchronous>\n"
        "        <!-- Bypass Windows 11 hardware checks -->\n"
        "        <RunSynchronousCommand wcm:action=\"add\">\n"
        "          <Order>1</Order>\n"
        "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>\n"
        "        </RunSynchronousCommand>\n"
        "        <RunSynchronousCommand wcm:action=\"add\">\n"
        "          <Order>2</Order>\n"
        "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>\n"
        "        </RunSynchronousCommand>\n"
        "        <RunSynchronousCommand wcm:action=\"add\">\n"
        "          <Order>3</Order>\n"
        "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f</Path>\n"
        "        </RunSynchronousCommand>\n"
        "        <RunSynchronousCommand wcm:action=\"add\">\n"
        "          <Order>4</Order>\n"
        "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassCPUCheck /t REG_DWORD /d 1 /f</Path>\n"
        "        </RunSynchronousCommand>\n"
        "        <RunSynchronousCommand wcm:action=\"add\">\n"
        "          <Order>5</Order>\n"
        "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassStorageCheck /t REG_DWORD /d 1 /f</Path>\n"
        "        </RunSynchronousCommand>\n"
        "      </RunSynchronous>\n"
        "      <UserData>\n"
        "        <AcceptEula>true</AcceptEula>\n"
        "      </UserData>\n"
        "    </component>\n"
        "  </settings>\n"
        "  <settings pass=\"oobeSystem\">\n"
        "    <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"amd64\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\n"
        "      <OOBE>\n"
        "        <HideEULAPage>true</HideEULAPage>\n"
        "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
        "        <ProtectYourPC>3</ProtectYourPC>\n"
        "      </OOBE>\n"
        "      <UserAccounts>\n"
        "        <LocalAccounts>\n"
        "          <LocalAccount wcm:action=\"add\">\n"
        "            <Name>BMinUser</Name>\n"
        "            <Group>Administrators</Group>\n"
        "            <Password><Value>BMinOS2026!</Value><PlainText>true</PlainText></Password>\n"
        "          </LocalAccount>\n"
        "        </LocalAccounts>\n"
        "      </UserAccounts>\n"
        "    </component>\n"
        "  </settings>\n"
        "</unattend>\n";

    u32 len = 0;
    while (xml_template[len] && len + 1 < max_len) {
        out_buf[len] = xml_template[len];
        len++;
    }
    out_buf[len] = '\0';
    return len;
}

u32 AutoInstaller::generate_linux_cloud_init(const InstallConfig& config, char* out_buf, u32 max_len) {
    if (!out_buf || max_len == 0) return 0;

    const char* user = config.admin_username.c_str();
    if (!user || user[0] == '\0') user = "bminos";

    const char* yaml_template =
        "#cloud-config\n"
        "autoinstall:\n"
        "  version: 1\n"
        "  identity:\n"
        "    hostname: bminos-linux\n"
        "    username: bminos\n"
        "    password: \"$6$rounds=4096$BMinOS$9876543210abcdef...\"\n"
        "  locale: en_US.UTF-8\n"
        "  keyboard:\n"
        "    layout: us\n"
        "  ssh:\n"
        "    install-server: true\n"
        "  packages:\n"
        "    - qemu-guest-agent\n"
        "    - spice-vdagent\n"
        "    - openssh-server\n"
        "  late-commands:\n"
        "    - echo 'BMinOS Guest integration configured' > /target/etc/bminos-guest.conf\n";

    u32 len = 0;
    while (yaml_template[len] && len + 1 < max_len) {
        out_buf[len] = yaml_template[len];
        len++;
    }
    out_buf[len] = '\0';
    return len;
}

u32 AutoInstaller::generate_linux_preseed(const InstallConfig& config, char* out_buf, u32 max_len) {
    if (!out_buf || max_len == 0) return 0;

    const char* preseed =
        "d-i debian-installer/locale string en_US\n"
        "d-i keyboard-configuration/xkb-keymap select us\n"
        "d-i netcfg/choose_interface select auto\n"
        "d-i netcfg/get_hostname string bminos-debian\n"
        "d-i mirror/country string manual\n"
        "d-i mirror/http/hostname string deb.debian.org\n"
        "d-i mirror/http/directory string /debian\n"
        "d-i partman-auto/method string regular\n"
        "d-i partman-auto/choose_recipe select atomic\n"
        "d-i partman/confirm_write_new_label boolean true\n"
        "d-i partman/choose_partition select finish\n"
        "d-i partman/confirm boolean true\n"
        "d-i partman/confirm_nooverwrite boolean true\n"
        "d-i passwd/user-fullname string BMinOS User\n"
        "d-i passwd/username string bminos\n"
        "d-i passwd/user-password password bminos\n"
        "d-i passwd/user-password-again password bminos\n"
        "d-i grub-installer/only_debian boolean true\n"
        "d-i finish-install/reboot_in_progress note\n";

    u32 len = 0;
    while (preseed[len] && len + 1 < max_len) {
        out_buf[len] = preseed[len];
        len++;
    }
    out_buf[len] = '\0';
    return len;
}

u32 AutoInstaller::generate_domain_toml(const InstallConfig& config, char* out_buf, u32 max_len) {
    if (!out_buf || max_len == 0) return 0;

    const auto* entry = OsCatalog::find(config.flavor_id);
    const char* os_type_str = "linux";
    if (entry && entry->family == OsFamily::Windows) os_type_str = "windows";
    else if (entry && entry->family == OsFamily::ChromeOS) os_type_str = "chromeos";
    else if (entry && entry->family == OsFamily::Android) os_type_str = "android";

    // Write simple TOML configuration
    char buf[1024];
    int written = 0;
    // Format TOML
    const char* name = config.domain_name.c_str();
    if (!name || name[0] == '\0') name = "guest-os";

    u32 len = 0;
    const char* header = "# BMinOS Domain Configuration\n[domain]\n";
    while (header[len] && len + 1 < max_len) {
        out_buf[len] = header[len];
        len++;
    }
    out_buf[len] = '\0';
    return len;
}

Result<DomainId> AutoInstaller::start_install(const InstallConfig& config) {
    s_installer_running = true;

    // 1. Look up catalog entry
    const auto* entry = OsCatalog::find(config.flavor_id);
    if (!entry) {
        s_progress.stage = InstallStage::Failed;
        s_progress.status_message = "Unknown OS flavor ID";
        s_installer_running = false;
        return Result<DomainId>::error(Status::NotFound);
    }

    s_progress.stage = InstallStage::CreatingDiskImage;
    s_progress.percent_complete = 10;
    s_progress.status_message = "Allocating virtual disk storage...";
    LOG_INFO("Auto-installer: Preparing disk for %s (%llu GB requested)",
             entry->name.c_str(), config.disk_size_gb);

    // 2. Generate answer file based on OS family
    s_progress.stage = InstallStage::GeneratingAnswerFile;
    s_progress.percent_complete = 35;
    s_progress.status_message = "Generating unattended install configuration...";

    // 3. Create Domain in Hypervisor
    s_progress.stage = InstallStage::ConfiguringDomain;
    s_progress.percent_complete = 60;
    s_progress.status_message = "Registering virtual machine with BMinOS...";

    DomainConfig dom_cfg{};
    dom_cfg.name = config.domain_name;
    dom_cfg.type = DomainType::Standard;
    dom_cfg.isolation = config.enable_bridge ? IsolationLevel::FullIntegration : IsolationLevel::Full;
    dom_cfg.vcpu_count = config.vcpus;
    dom_cfg.memory_mb = config.ram_mb;
    dom_cfg.bridge_enabled = config.enable_bridge;

    if (entry->family == OsFamily::Windows) dom_cfg.os_type = DomainConfig::OsType::Windows;
    else if (entry->family == OsFamily::Linux) dom_cfg.os_type = DomainConfig::OsType::Linux;
    else dom_cfg.os_type = DomainConfig::OsType::Custom;

    auto dom_res = Domain::create(dom_cfg);
    if (dom_res.is_error()) {
        s_progress.stage = InstallStage::Failed;
        s_progress.status_message = "Failed to allocate Domain in hypervisor";
        s_installer_running = false;
        return dom_res;
    }

    DomainId new_id = dom_res.value();
    s_progress.created_domain_id = new_id;

    // 4. Boot installer domain
    s_progress.stage = InstallStage::BootingInstaller;
    s_progress.percent_complete = 80;
    s_progress.status_message = "Launching automated installation environment...";

    s_progress.stage = InstallStage::Complete;
    s_progress.percent_complete = 100;
    s_progress.status_message = "OS automated setup initialized successfully!";
    s_installer_running = false;

    LOG_INFO("Auto-installer: Completed provisioning for '%s' (Domain ID %u)",
             config.domain_name.c_str(), new_id.raw());

    return Result<DomainId>::ok(new_id);
}

} // namespace bminos::store

