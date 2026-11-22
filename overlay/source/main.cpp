/**
 * @file main.cpp
 * @brief Entry point for the ryu_ldn_nx Tesla Overlay.
 *
 * Initializes the ryu:cfg IPC service, queries the sysmodule version,
 * and loads the MainDashboardGui as the initial screen.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include "ryu_ldn_ipc.h"
#include "app/overlay_state.hpp"
#include "views/connection/main_dashboard_gui.hpp"

/**
 * @brief Tesla overlay object: owns the ryu:cfg service session lifetime.
 */
class RyuLdnOverlay : public tsl::Overlay {
public:
    /**
     * @brief Connects to ryu:cfg and caches its version in OverlayState.
     *
     * Runs inside an sm session; the status ends as Error when the
     * connection fails, Loaded otherwise (version "Unknown" if the version
     * query fails).
     */
    virtual void initServices() override {
        auto& state = OverlayState::Instance();
        state.SetStatus(OverlayState::InitStatus::Uninit);
        tsl::hlp::doWithSmSession([&] {
            Result rc = ryuLdnInitialize();
            if (R_FAILED(rc)) {
                state.SetStatus(OverlayState::InitStatus::Error);
                return;
            }
            RyuLdnConfigService* svc = ryuLdnGetService();
            if (svc) {
                char version[32];
                rc = ryuLdnGetVersion(svc, version);
                state.SetVersion(R_SUCCEEDED(rc) ? version : "Unknown");
            }
            state.SetStatus(OverlayState::InitStatus::Loaded);
        });
    }
    /// Closes the ryu:cfg session if initServices() loaded it.
    virtual void exitServices() override {
        if (OverlayState::Instance().GetStatus() ==
            OverlayState::InitStatus::Loaded)
            ryuLdnExit();
    }
    /// Overlay became visible; nothing to do.
    virtual void onShow() override {}
    /// Overlay was hidden; nothing to do.
    virtual void onHide() override {}
    /// @return The MainDashboardGui shown when the overlay opens.
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainDashboardGui>();
    }
};

/**
 * @brief Overlay entry point; runs the Tesla loop for RyuLdnOverlay.
 *
 * @param argc Argument count forwarded to Tesla.
 * @param argv Argument vector forwarded to Tesla.
 * @return Exit code of tsl::loop.
 */
int main(int argc, char** argv) {
    return tsl::loop<RyuLdnOverlay>(argc, argv);
}
