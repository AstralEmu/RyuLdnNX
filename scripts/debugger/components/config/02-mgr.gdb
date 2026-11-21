# =========================================
# CONFIG:MGR
# =========================================

echo [CONFIG] Loading mgr breakpoints...\n
# Namespace: ryu_ldn::config
dprintf ryu_ldn::config::IsValidPassphrase, "[CONFIG:MGR] Validating passphrase\n"
dprintf ryu_ldn::config::GenerateRandomPassphrase, "[CONFIG:MGR] Generating random passphrase\n"
dprintf ryu_ldn::config::ConfigManager::Instance, "[CONFIG:MGR] Getting manager instance\n"
dprintf ryu_ldn::config::ConfigManager::Initialize, "[CONFIG:MGR] Initializing configuration manager\n"
dprintf ryu_ldn::config::ConfigManager::Save, "[CONFIG:MGR] Saving configuration to disk\n"
dprintf ryu_ldn::config::ConfigManager::Reload, "[CONFIG:MGR] Reloading configuration from disk\n"
dprintf ryu_ldn::config::ConfigManager::SetServerHost, "[CONFIG:MGR] Setting server host\n"
dprintf ryu_ldn::config::ConfigManager::SetServerPort, "[CONFIG:MGR] Setting server port\n"
dprintf ryu_ldn::config::ConfigManager::SetLdnEnabled, "[CONFIG:MGR] Setting LDN enabled\n"
dprintf ryu_ldn::config::ConfigManager::SetPassphrase, "[CONFIG:MGR] Setting passphrase\n"
dprintf ryu_ldn::config::ConfigManager::SetUsePassphrase, "[CONFIG:MGR] Setting use_passphrase\n"
dprintf ryu_ldn::config::ConfigManager::SetDebugEnabled, "[CONFIG:MGR] Setting debug enabled\n"
dprintf ryu_ldn::config::ConfigManager::SetDebugLevel, "[CONFIG:MGR] Setting debug level\n"
dprintf ryu_ldn::config::ConfigManager::SetChangeCallback, "[CONFIG:MGR] Setting change callback\n"
dprintf ryu_ldn::config::ConfigManager::NotifyChange, "[CONFIG:MGR] Config change notified\n"

echo [CONFIG] mgr: 15 dprintf points\n
