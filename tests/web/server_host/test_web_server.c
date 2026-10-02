/* Compile the real dispatcher against narrow host fakes. No ESP-IDF install
 * or board is needed. The fakes intentionally do not model lwIP or task races. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../main/web/web_server.c"

static outgoing_t *queue[WEB_PENDING_LIMIT];
static unsigned queued;
static bool queue_fail, send_fail, schedule_fail, stop_after_schedule;
static unsigned sends, shutdowns, scheduled;
static int shutdown_fd;
static httpd_work_fn_t captured_fn;
static void *captured_arg;
static jmp_buf dispatch_exit;

void *heap_caps_malloc(size_t count, int caps)
{
    (void)caps;
    return malloc(count);
}

int xQueueSend(QueueHandle_t handle, const void *item, unsigned wait)
{
    (void)handle;
    assert(wait == 0); /* Controller submission must never wait. */
    if (queue_fail || queued == WEB_PENDING_LIMIT) return 0;
    queue[queued++] = *(outgoing_t *const *)item;
    return pdTRUE;
}

int xQueueReceive(QueueHandle_t handle, void *item, unsigned wait)
{
    (void)handle;
    (void)wait;
    if (!queued) {
        atomic_store(&s_dispatch_stop, true);
        return 0;
    }
    *(outgoing_t **)item = queue[0];
    memmove(queue, queue + 1, --queued * sizeof(*queue));
    return pdTRUE;
}

int httpd_ws_get_fd_info(httpd_handle_t server, int fd)
{
    (void)server;
    (void)fd;
    return HTTPD_WS_CLIENT_WEBSOCKET;
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t server, int fd,
                                   httpd_ws_frame_t *frame)
{
    (void)server;
    (void)fd;
    assert(frame->len == 3);
    assert(memcmp(frame->payload, "abc", 3) == 0);
    ++sends;
    return send_fail ? ESP_FAIL : ESP_OK;
}

esp_err_t httpd_queue_work(httpd_handle_t server, httpd_work_fn_t fn, void *arg)
{
    (void)server;
    ++scheduled;
    captured_fn = fn;
    captured_arg = arg;
    return schedule_fail ? ESP_FAIL : ESP_OK;
}

void vTaskDelay(unsigned ticks)
{
    (void)ticks;
    if (stop_after_schedule) longjmp(dispatch_exit, 1);
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

int shutdown(int fd, int how)
{
    assert(how == SHUT_RDWR);
    ++shutdowns;
    shutdown_fd = fd;
    return 0;
}

static void reset_(void)
{
    assert(atomic_load(&s_pending) == 0);
    memset(s_peers, 0, sizeof(s_peers));
    s_peers[0] = (peer_t){ .id = 42, .fd = 11 };
    s_server = (void *)1;
    s_outgoing = (void *)1;
    atomic_store(&s_ready, true);
    atomic_store(&s_work_pending, false);
    atomic_store(&s_dispatch_stop, false);
    queue_fail = send_fail = schedule_fail = stop_after_schedule = false;
    queued = sends = shutdowns = scheduled = 0;
    captured_fn = NULL;
    captured_arg = NULL;
}

static void run_one_dispatch_(void)
{
    stop_after_schedule = true;
    if (!setjmp(dispatch_exit)) dispatch_task_(NULL);
    stop_after_schedule = false;
}

static void test_full_queue_and_reused_fd_(void)
{
    reset_();
    assert(web_server_peer_alive(42));
    assert(!web_server_peer_alive(0));
    assert(!web_server_send(41, "abc", 3));
    for (unsigned i = 0; i < WEB_PENDING_LIMIT; ++i)
        assert(web_server_send(42, "abc", 3));
    assert(queued == WEB_PENDING_LIMIT);
    assert(atomic_load(&s_pending) == WEB_PENDING_LIMIT);
    assert(!web_server_send(42, "abc", 3));
    assert(atomic_load(&s_pending) == WEB_PENDING_LIMIT);

    /* A full output queue must not lose or duplicate the close request. */
    web_server_close(42);
    assert(!web_server_peer_alive(42));
    assert(next_close_() == 42);
    assert(next_close_() == -1);
    retry_close_(42);
    assert(next_close_() == 42);
    close_work_((void *)(intptr_t)42);
    assert(shutdowns == 1 && shutdown_fd == 11 && scheduled == 0);

    /* The same fd now belongs to a different logical connection. */
    s_peers[0] = (peer_t){ .id = 43, .fd = 11 };
    close_work_((void *)(intptr_t)42);
    assert(shutdowns == 1);
    while (queued) send_work_(queue[--queued]);
    assert(sends == 0 && atomic_load(&s_pending) == 0);
}

static void test_copy_and_callback_accounting_(void)
{
    reset_();
    char payload[] = "abc";
    assert(web_server_send(42, payload, 3));
    memset(payload, 'x', 3);
    run_one_dispatch_();
    assert(scheduled == 1 && queued == 0);
    assert(atomic_load(&s_pending) == 1 && atomic_load(&s_work_pending));

    /* The callback handed to HTTPD still consumes one of the eight slots. */
    for (unsigned i = 1; i < WEB_PENDING_LIMIT; ++i)
        assert(web_server_send(42, "abc", 3));
    assert(!web_server_send(42, "abc", 3));
    captured_fn(captured_arg); /* Asserts copied payload still equals abc. */
    assert(sends == 1 && !atomic_load(&s_work_pending));
    while (queued) discard_output_(queue[--queued]);
    assert(atomic_load(&s_pending) == 0);
}

static void test_error_cleanup_(void)
{
    reset_();
    queue_fail = true;
    assert(!web_server_send(42, "abc", 3));
    assert(atomic_load(&s_pending) == 0);

    reset_();
    assert(web_server_send(42, "abc", 3));
    run_one_dispatch_();
    send_fail = true;
    captured_fn(captured_arg);
    assert(sends == 1 && !web_server_peer_alive(42));
    assert(atomic_load(&s_pending) == 0 && !atomic_load(&s_work_pending));
    assert(scheduled == 1); /* Failure must not recursively queue a close. */

    reset_();
    assert(web_server_send(42, "abc", 3));
    schedule_fail = true;
    dispatch_task_(NULL);
    assert(atomic_load(&s_pending) == 0 && !atomic_load(&s_work_pending));
}

static void test_close_priority_and_retry_(void)
{
    reset_();
    assert(web_server_send(42, "abc", 3));
    web_server_close(42);
    run_one_dispatch_();
    assert(scheduled == 1 && captured_fn == close_work_ && queued == 1);
    captured_fn(captured_arg);
    discard_output_(queue[--queued]);

    reset_();
    web_server_close(42);
    schedule_fail = true;
    run_one_dispatch_();
    assert(scheduled == 1 && !s_peers[0].close_queued && s_peers[0].closing);
    assert(!atomic_load(&s_work_pending));
}

int main(void)
{
    test_full_queue_and_reused_fd_();
    test_copy_and_callback_accounting_();
    test_error_cleanup_();
    test_close_priority_and_retry_();
    puts("PASS: bounded queue, payload copy, error cleanup, logical FD identity, "
         "close retention/priority, callback accounting and retry");
    return 0;
}
