# =========================================
# DEBUG:LOGGER
# =========================================

echo [DEBUG] Loading logger breakpoints...\n
# Namespace: ryu_ldn::debug
dprintf ryu_ldn::debug::Logger::init, "[DEBUG:LOGGER] Logger initialized\n"
dprintf ryu_ldn::debug::Logger::SetEnabled, "[DEBUG:LOGGER] SetEnabled\n"
dprintf ryu_ldn::debug::Logger::SetLevel, "[DEBUG:LOGGER] SetLevel\n"
dprintf ryu_ldn::debug::Logger::open_file, "[DEBUG:LOGGER] Log file opened\n"
dprintf ryu_ldn::debug::Logger::close_file, "[DEBUG:LOGGER] Log file closed\n"

echo [DEBUG] logger: 5 dprintf points\n
