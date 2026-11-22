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

struct UPNPUrls {
    char* controlURL;
    char* rootdescURL;
};

struct IGDdatas_service {
    char servicetype[64];
};

struct IGDdatas {
    IGDdatas_service first;
};

struct UPNPDev {
    UPNPDev* pNext;
};

#define UPNPCOMMAND_SUCCESS (0)

/// Scripted answers and recorded calls of the fake library.
struct FakeUpnp {
    // answers
    bool devices_found = true;     ///< upnpDiscover returns a device list
    int igd_result = 1;            ///< UPNP_GetValidIGD return value
    bool igd_sets_control = true;  ///< UPNP_GetValidIGD fills the control URL
    std::string lan_addr = "192.168.1.100";
    std::string service_type = "urn:fake:WANIPConnection:1";
    int add_result = 0;
    int delete_result = 0;
    int external_ip_result = 0;
    std::string external_ip = "203.0.113.7";
    // recorded
    int discover_calls = 0;
    int add_calls = 0;
    int devlist_frees = 0;
    int urls_frees = 0;
    int last_delay = 0;
    std::string add_ext_port, add_int_port, add_client, add_desc, add_proto,
        add_lease;
    bool add_remote_host_null = false;
    std::string del_ext_port, del_proto;
    bool del_remote_host_null = false;
    std::string used_service_type;
};
inline FakeUpnp g_fake_upnp;

inline UPNPDev* upnpDiscover(
    int delay, const char*, const char*, int, int, unsigned char, int* error) {
    ++g_fake_upnp.discover_calls;
    g_fake_upnp.last_delay = delay;
    *error = g_fake_upnp.devices_found ? 0 : -1;
    return g_fake_upnp.devices_found
               ? static_cast<UPNPDev*>(std::calloc(1, sizeof(UPNPDev)))
               : nullptr;
}

inline void freeUPNPDevlist(UPNPDev* devlist) {
    ++g_fake_upnp.devlist_frees;
    std::free(devlist);
}

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

inline void FreeUPNPUrls(UPNPUrls* urls) {
    ++g_fake_upnp.urls_frees;
    std::free(urls->controlURL);
    urls->controlURL = nullptr;
}

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

inline int UPNP_GetExternalIPAddress(
    const char*, const char* servicetype, char* ext_ip) {
    g_fake_upnp.used_service_type = servicetype;
    std::strncpy(ext_ip, g_fake_upnp.external_ip.c_str(), 15);
    return g_fake_upnp.external_ip_result;
}
