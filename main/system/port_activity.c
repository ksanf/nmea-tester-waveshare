#include "system/port_activity.h"
#include <stdatomic.h>

static atomic_uint s_pending;

void port_activity_mark(port_activity_channel_t channel)
{
    if ((unsigned)channel >= PORT_ACTIVITY_COUNT) return;
    atomic_fetch_or_explicit(&s_pending, 1u << channel, memory_order_relaxed);
}

uint32_t port_activity_take(void)
{
    return atomic_exchange_explicit(&s_pending, 0, memory_order_relaxed);
}
