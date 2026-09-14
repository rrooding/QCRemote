#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

namespace qcbridge {

// Bring-up sanity check for the frdm_rw612 toolchain/build (issue #2) - not
// part of the actual bridge firmware. Confirms the board boots, GPIO and
// logging work, and the app links against a real C++23 toolchain.
class HeartbeatLed {
public:
    explicit HeartbeatLed(const gpio_dt_spec& spec) : spec_(spec) {}

    bool init() const {
        if (!gpio_is_ready_dt(&spec_)) {
            return false;
        }
        return gpio_pin_configure_dt(&spec_, GPIO_OUTPUT_ACTIVE) == 0;
    }

    void toggle() const { gpio_pin_toggle_dt(&spec_); }

private:
    gpio_dt_spec spec_;
};

}  // namespace qcbridge

namespace {
const gpio_dt_spec kLedSpec = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
const auto kBlinkInterval = K_MSEC(500);
}  // namespace

int main() {
    const qcbridge::HeartbeatLed led(kLedSpec);

    if (!led.init()) {
        LOG_ERR("Failed to initialize heartbeat LED");
        return 0;
    }

    LOG_INF("QC Bridge firmware skeleton up (C++%ld)", static_cast<long>(__cplusplus));

    while (true) {
        led.toggle();
        k_sleep(kBlinkInterval);
    }
}
