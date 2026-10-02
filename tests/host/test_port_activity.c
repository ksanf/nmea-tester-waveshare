#include "system/port_activity.h"
#include "ui/activity_pulse.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_uint s_done;
static void *writer(void *arg)
{
    port_activity_channel_t channel = *(port_activity_channel_t *)arg;
    for (unsigned i = 0; i < 10000; ++i) port_activity_mark(channel);
    atomic_fetch_add(&s_done, 1);
    return NULL;
}

int main(void)
{
    assert(port_activity_take() == 0);
    port_activity_mark(PORT_ACTIVITY_RS485_TX);
    port_activity_mark(PORT_ACTIVITY_RS485_TX);
    port_activity_mark(PORT_ACTIVITY_CAN_RX);
    assert(port_activity_take() == ((1u << PORT_ACTIVITY_RS485_TX) | (1u << PORT_ACTIVITY_CAN_RX)));
    assert(port_activity_take() == 0);
    port_activity_mark((port_activity_channel_t)-1);
    port_activity_mark(PORT_ACTIVITY_COUNT);
    assert(port_activity_take() == 0);

    pthread_t writers[PORT_ACTIVITY_COUNT];
    port_activity_channel_t channels[PORT_ACTIVITY_COUNT];
    for (unsigned i = 0; i < PORT_ACTIVITY_COUNT; ++i) {
        channels[i] = (port_activity_channel_t)i;
        assert(pthread_create(&writers[i], NULL, writer, &channels[i]) == 0);
    }
    uint32_t observed = 0;
    while (atomic_load(&s_done) != PORT_ACTIVITY_COUNT) observed |= port_activity_take();
    for (unsigned i = 0; i < PORT_ACTIVITY_COUNT; ++i) assert(pthread_join(writers[i], NULL) == 0);
    observed |= port_activity_take();
    assert(observed == (1u << PORT_ACTIVITY_COUNT) - 1);

    activity_pulse_t pulse;
    activity_pulse_reset(&pulse, 1000);
    assert(!activity_pulse_update(&pulse, false, 1000));
    assert(activity_pulse_update(&pulse, true, 1000));
    assert(activity_pulse_update(&pulse, false, 1099));
    assert(!activity_pulse_update(&pulse, false, 1100));
    assert(!activity_pulse_update(&pulse, false, 2000));
    assert(activity_pulse_update(&pulse, true, 2001));
    assert(activity_pulse_update(&pulse, true, 2050));
    assert(!activity_pulse_update(&pulse, true, 2101));
    assert(!activity_pulse_update(&pulse, true, 2180));
    assert(activity_pulse_update(&pulse, false, 2181));
    assert(!activity_pulse_update(&pulse, false, 2281));
    assert(!activity_pulse_update(&pulse, false, 10000));

    activity_pulse_reset(&pulse, UINT32_MAX - 40u);
    assert(activity_pulse_update(&pulse, true, UINT32_MAX - 40u));
    assert(activity_pulse_update(&pulse, false, 58u));
    assert(!activity_pulse_update(&pulse, false, 59u));
    activity_pulse_reset(&pulse, 60u);
    assert(!activity_pulse_update(&pulse, false, 60u));
    assert(activity_pulse_update(&pulse, true, 60u));

    /* At continuous load the lamp must have visible dark gaps, then settle off. */
    activity_pulse_reset(&pulse, 0);
    unsigned rises = 0;
    bool was_lit = false;
    for (uint32_t now = 0; now < 2000; now += 40) {
        bool lit = activity_pulse_update(&pulse, true, now);
        if (lit && !was_lit) ++rises;
        was_lit = lit;
    }
    assert(rises >= 9 && rises <= 11);
    for (uint32_t now = 2000; now <= 2400; now += 40) activity_pulse_update(&pulse, false, now);
    assert(!pulse.lit && !pulse.pending);
    puts("port activity: concurrency, burst coalescing, pulse timing and tick wrap PASS");
    return 0;
}
