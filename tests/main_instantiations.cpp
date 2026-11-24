/**
 * @file main_instantiations.cpp
 * @brief Explicit instantiations of the stub server manager for boot tests
 *
 * The stub server manager is a class template with a virtual accept hook.
 * Its vtable, and therefore the hook's definition, lives in main.cpp; without
 * an explicit instantiation the boot test translation unit would only see
 * the hook declared, which clang-tidy's
 * portability-template-virtual-member-function rightly refuses. The two
 * specializations the boot tests drive are instantiated here, in a file of
 * their own.
 */

#include "main_internal.hpp"

namespace boot = ams;

namespace {
/// Port count of the boot MITM server manager.
constexpr auto kMitmPorts = boot::internal::MITM_PORT_COUNT;
/// Port count of the config server manager.
constexpr auto kCfgPorts = boot::internal::CFG_PORT_COUNT;
/// Options of the boot MITM server manager.
using MitmOptions = boot::mitm::LdnMitmManagerOptions;
/// Options of the config server manager.
using CfgOptions = boot::cfg::ConfigServerManagerOptions;
}  // namespace

template class ams::sf::hipc::
    ServerManager<kMitmPorts, MitmOptions, boot::mitm::MaxSessions>;
template class ams::sf::hipc::
    ServerManager<kCfgPorts, CfgOptions, boot::cfg::MaxSessions>;
