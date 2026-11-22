/**
 * @file miniupnpc.h
 * @brief Host-side stand-in for the miniupnpc C API used by UpnpPortMapper
 *
 * On the include path of tests/Makefile only (-Istubs). It reproduces the
 * signatures and the ownership contract of the functions the sysmodule calls
 * (the control URL is heap allocated by UPNP_GetValidIGD and released by
 * FreeUPNPUrls), and lets a test script every answer and read back every
 * argument through g_fake_upnp.
 */

#pragma once

#include <cstdlib>
#include <cstring>
#include <string>

/// Subset of miniupnpc's UPNPUrls: the URLs of the discovered IGD.
struct UPNPUrls {
    /// SOAP control URL; malloc'ed by UPNP_GetValidIGD, freed by FreeUPNPUrls.
    char* controlURL;
    /// Root description URL (never filled by this stub).
    char* rootdescURL;
};

/// Subset of miniupnpc's IGDdatas service description.
struct IGDdatas_service {
    /// Service type URN of the selected IGD service, NUL-terminated.
    char servicetype[64];
};

/// Subset of miniupnpc's IGDdatas: description data of the discovered IGD.
struct IGDdatas {
    /// First (and only) service of the stubbed device.
    IGDdatas_service first;
};

/// Node of the device list returned by upnpDiscover (single node here).
struct UPNPDev {
    /// Next device of the list; always nullptr since the stub returns one node.
    UPNPDev* pNext;
};

/// Return value of the UPNP_*PortMapping/ExternalIP calls on success.
#define UPNPCOMMAND_SUCCESS (0)

/// Scripted answers and recorded calls of the fake library.
struct FakeUpnp {
    // answers
    bool devices_found = true;     ///< upnpDiscover returns a device list
    int igd_result = 1;            ///< UPNP_GetValidIGD return value
    bool igd_sets_control = true;  ///< UPNP_GetValidIGD fills the control URL
    /// LAN address UPNP_GetValidIGD writes into lanaddr.
    std::string lan_addr = "192.168.1.100";
    /// Service type UPNP_GetValidIGD writes into IGDdatas.
    std::string service_type = "urn:fake:WANIPConnection:1";
    /// UPNP_AddPortMapping return value (0 = UPNPCOMMAND_SUCCESS).
    int add_result = 0;
    /// UPNP_DeletePortMapping return value (0 = UPNPCOMMAND_SUCCESS).
    int delete_result = 0;
    /// UPNP_GetExternalIPAddress return value (0 = UPNPCOMMAND_SUCCESS).
    int external_ip_result = 0;
    /// External IP copied out by UPNP_GetExternalIPAddress (TEST-NET-3 range).
    std::string external_ip = "203.0.113.7";
    // recorded
    /// Number of upnpDiscover calls.
    int discover_calls = 0;
    /// Number of UPNP_AddPortMapping calls.
    int add_calls = 0;
    /// Number of freeUPNPDevlist calls.
    int devlist_frees = 0;
    /// Number of FreeUPNPUrls calls.
    int urls_frees = 0;
    /// Discovery delay (ms) passed to the last upnpDiscover call.
    int last_delay = 0;
    // Arguments of the last UPNP_AddPortMapping call (all strings in the API)
    std::string add_ext_port,  ///< External port of the last AddPortMapping.
        add_int_port,          ///< Internal port of the last AddPortMapping.
        add_client,            ///< Internal client address of the last add.
        add_desc,              ///< Mapping description of the last add.
        add_proto,             ///< Protocol ("TCP"/"UDP") of the last add.
        add_lease;             ///< Lease duration string of the last add.
    /// Whether the last UPNP_AddPortMapping got a null remote host.
    bool add_remote_host_null = false;
    std::string del_ext_port,  ///< External port of the last delete.
        del_proto;             ///< Protocol of the last delete.
    /// Whether the last UPNP_DeletePortMapping got a null remote host.
    bool del_remote_host_null = false;
    /// Service type string passed by the last add/delete/external-IP call.
    std::string used_service_type;
};
/// Single instance every stubbed miniupnpc function reads and writes.
inline FakeUpnp g_fake_upnp;

/**
 * @brief Fake device discovery.
 *
 * Counts the call, records the delay and reports success or failure per
 * FakeUpnp::devices_found.
 *
 * @param delay Discovery timeout in ms, recorded in FakeUpnp::last_delay.
 * @param[out] error Set to 0 when devices are found, -1 otherwise.
 * @return A calloc'ed one-node device list (release it with freeUPNPDevlist),
 *         or nullptr when FakeUpnp::devices_found is false.
 */
inline UPNPDev* upnpDiscover(
    int delay, const char*, const char*, int, int, unsigned char, int* error) {
    ++g_fake_upnp.discover_calls;
    g_fake_upnp.last_delay = delay;
    *error = g_fake_upnp.devices_found ? 0 : -1;
    return g_fake_upnp.devices_found
               ? static_cast<UPNPDev*>(std::calloc(1, sizeof(UPNPDev)))
               : nullptr;
}

/**
 * @brief Fake release of a device list: counts the call and frees the node.
 * @param devlist List returned by upnpDiscover (nullptr is accepted).
 */
inline void freeUPNPDevlist(UPNPDev* devlist) {
    ++g_fake_upnp.devlist_frees;
    std::free(devlist);
}

/**
 * @brief Fake IGD selection.
 *
 * Optionally allocates the control URL, always fills the service type and the
 * LAN address from FakeUpnp.
 *
 * @param[out] urls Receives a malloc'ed controlURL when
 *             FakeUpnp::igd_sets_control is true.
 * @param[out] data Receives FakeUpnp::service_type (truncated to 63 chars).
 * @param[out] lanaddr Receives FakeUpnp::lan_addr (truncated to fit).
 * @param lanaddrlen Capacity of lanaddr in bytes.
 * @return FakeUpnp::igd_result (1 = valid connected IGD in real miniupnpc).
 */
inline int UPNP_GetValidIGD(
    UPNPDev*, UPNPUrls* urls, IGDdatas* data, char* lanaddr, int lanaddrlen) {
    if (g_fake_upnp.igd_sets_control) {
        urls->controlURL = static_cast<char*>(std::malloc(32));
        std::strcpy(urls->controlURL, "http://192.168.1.1/ctl");
    }
    std::strncpy(
        data->first.servicetype,
        g_fake_upnp.service_type.c_str(),
        sizeof(data->first.servicetype) - 1);
    std::strncpy(
        lanaddr,
        g_fake_upnp.lan_addr.c_str(),
        static_cast<std::size_t>(lanaddrlen) - 1);
    return g_fake_upnp.igd_result;
}

/**
 * @brief Fake release of the IGD URLs: counts the call and frees controlURL.
 * @param[in,out] urls URLs to release; controlURL is reset to nullptr.
 */
inline void FreeUPNPUrls(UPNPUrls* urls) {
    ++g_fake_upnp.urls_frees;
    std::free(urls->controlURL);
    urls->controlURL = nullptr;
}

/**
 * @brief Fake port mapping creation: records every argument in g_fake_upnp.
 * @param servicetype Service type, recorded in FakeUpnp::used_service_type.
 * @param ext_port External port, as a string.
 * @param in_port Internal port, as a string.
 * @param in_client Internal client address.
 * @param desc Mapping description.
 * @param proto Protocol ("TCP" or "UDP").
 * @param remote_host Remote host filter; nullptr means any.
 * @param lease Lease duration in seconds, as a string.
 * @return FakeUpnp::add_result.
 */
inline int UPNP_AddPortMapping(
    const char*,
    const char* servicetype,
    const char* ext_port,
    const char* in_port,
    const char* in_client,
    const char* desc,
    const char* proto,
    const char* remote_host,
    const char* lease) {
    g_fake_upnp.used_service_type = servicetype;
    ++g_fake_upnp.add_calls;
    g_fake_upnp.add_ext_port = ext_port;
    g_fake_upnp.add_int_port = in_port;
    g_fake_upnp.add_client = in_client;
    g_fake_upnp.add_desc = desc;
    g_fake_upnp.add_proto = proto;
    g_fake_upnp.add_remote_host_null = (remote_host == nullptr);
    g_fake_upnp.add_lease = lease;
    return g_fake_upnp.add_result;
}

/**
 * @brief Fake port mapping removal: records the arguments in g_fake_upnp.
 * @param servicetype Service type, recorded in FakeUpnp::used_service_type.
 * @param ext_port External port, as a string.
 * @param proto Protocol ("TCP" or "UDP").
 * @param remote_host Remote host filter; nullptr means any.
 * @return FakeUpnp::delete_result.
 */
inline int UPNP_DeletePortMapping(
    const char*,
    const char* servicetype,
    const char* ext_port,
    const char* proto,
    const char* remote_host) {
    g_fake_upnp.used_service_type = servicetype;
    g_fake_upnp.del_ext_port = ext_port;
    g_fake_upnp.del_proto = proto;
    g_fake_upnp.del_remote_host_null = (remote_host == nullptr);
    return g_fake_upnp.delete_result;
}

/**
 * @brief Fake external IP query.
 * @param servicetype Service type, recorded in FakeUpnp::used_service_type.
 * @param[out] ext_ip Receives FakeUpnp::external_ip, copied with strncpy of 15
 *             bytes (the caller must provide a 16-byte buffer; the copy is
 *             not NUL-terminated for 15-character addresses unless the buffer
 *             was zeroed).
 * @return FakeUpnp::external_ip_result.
 */
inline int UPNP_GetExternalIPAddress(
    const char*, const char* servicetype, char* ext_ip) {
    g_fake_upnp.used_service_type = servicetype;
    std::strncpy(ext_ip, g_fake_upnp.external_ip.c_str(), 15);
    return g_fake_upnp.external_ip_result;
}
