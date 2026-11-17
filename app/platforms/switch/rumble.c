#include "platform/rumble.h"
#include <SDL.h>
#include <switch.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

typedef struct binding {
    HidVibrationDeviceHandle handles[2];
    HidNpadIdType id;
    u32 style;
    unsigned count;
    uint64_t generation;
} binding;
typedef struct slot {
    binding target;
    sl_haptic_stream stream;
} slot;
typedef struct output {
    binding target;
    sl_haptic_frame frame;
    unsigned slot;
    bool stop;
} output;

static slot slots[8];
static pthread_t worker;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool initialized, available, stopping, ready;
static sl_rumble_snapshot stats;
#ifndef NSL_RUMBLE_TEST
static uint64_t resource_id;
#endif
#if NSL_DIAGNOSTICS || defined(NSL_RUMBLE_TEST)
#define TRACE_CAP 512u
static sl_rumble_trace trace[TRACE_CAP];
static size_t trace_head, trace_count;
#endif

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void record(const sl_rumble_trace *r) {
#if NSL_DIAGNOSTICS || defined(NSL_RUMBLE_TEST)
    if (trace_count == TRACE_CAP)
        ++stats.trace_overwritten;
    else
        ++trace_count;
    trace[trace_head] = *r;
    trace[trace_head].serial = trace_count + stats.trace_overwritten;
    trace_head = (trace_head + 1) % TRACE_CAP;
#else
    (void)r;
#endif
}

/* Separate connection: no worker call enters SDL or libnx's global HID session.
 * Matches libnx hidSendVibrationValues (hid IPC 206). Device activation remains
 * on the SDL owner via hidInitializeVibrationDevices. */
typedef struct native_io {
#ifndef NSL_RUMBLE_TEST
    Service service;
#endif
    bool open;
} native_io;
_Static_assert(sizeof(sl_hd_value) == sizeof(HidVibrationValue), "HID vibration value layout");
_Static_assert(offsetof(sl_hd_value, freq_high) == offsetof(HidVibrationValue, freq_high),
               "HID vibration field order");
static Result io_open(native_io *io) {
#ifdef NSL_RUMBLE_TEST
    io->open = true;
    return 0;
#else
    Result rc = smGetService(&io->service, "hid");
    io->open = R_SUCCEEDED(rc);
    return rc;
#endif
}
static void io_close(native_io *io) {
#ifndef NSL_RUMBLE_TEST
    if (io->open)
        serviceClose(&io->service);
#endif
    io->open = false;
}
static Result io_send(native_io *io, const binding *b, const sl_hd_value values[2]) {
    HidVibrationValue v[2];
    for (unsigned i = 0; i < b->count; ++i) {
        unsigned source = b->count == 1 && (b->style & HidNpadStyleTag_NpadJoyRight) ? 1 : i;
        memcpy(&v[i], &values[source], sizeof(v[i]));
    }
#ifdef NSL_RUMBLE_TEST
    (void)io;
    return hidSendVibrationValues(b->handles, v, (int)b->count);
#else
    return serviceDispatchIn(&io->service, 206, resource_id,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In,
                         SfBufferAttr_HipcPointer | SfBufferAttr_In},
        .buffers = {{b->handles, b->count * sizeof(b->handles[0])},
                    {v, b->count * sizeof(v[0])}});
#endif
}
static Result emit(native_io *io, const output *o) {
    if (!o->stop) {
        pthread_mutex_lock(&lock);
        const slot *s = &slots[o->slot];
        bool current = !stopping && s->target.generation == o->target.generation &&
            s->stream.current.sequence == o->frame.sequence &&
            s->stream.current.sample == o->frame.sample &&
            s->stream.current.native == o->frame.native &&
            s->stream.current.due_ns == o->frame.due_ns;
        if (!current)
            ++stats.canceled_outputs;
        pthread_mutex_unlock(&lock);
        if (!current)
            return 0; /* Selected earlier, but never submitted to hardware. */
    }
    sl_hd_value zero[2] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
    const sl_hd_value *values = o->stop ? zero : o->frame.values;
    uint64_t submitted = now_ns();
    Result rc = io_send(io, &o->target, values);
    uint64_t completed = now_ns();
    pthread_mutex_lock(&lock);
    ++stats.sends;
    if (R_FAILED(rc)) {
        ++stats.send_failures;
        if (o->stop)
            ++stats.stop_failures;
        stats.io_result = rc;
    }
    slot *s = &slots[o->slot];
    if (!o->stop)
        sl_haptic_output_complete(&s->stream, &o->frame, submitted, completed, R_SUCCEEDED(rc),
                                  s->target.generation == o->target.generation);
    sl_rumble_trace t = {.kind = o->stop ? 3 : 2, .slot = o->slot,
        .received_ns = o->frame.received_ns, .due_ns = o->frame.due_ns,
        .submitted_ns = submitted, .completed_ns = completed, .sequence = o->frame.sequence,
        .sample = o->frame.sample, .result = rc, .count = o->target.count};
    memcpy(t.values, values, sizeof(t.values));
    record(&t);
    pthread_mutex_unlock(&lock);
    return rc;
}

static void *run(void *unused) {
    (void)unused;
    native_io io = {0};
    Result rc = io_open(&io);
    binding live[8] = {0};
    pthread_mutex_lock(&lock);
    stats.io_result = rc;
    ready = true;
    stats.worker_running = R_SUCCEEDED(rc);
    pthread_cond_broadcast(&wake);
    if (R_FAILED(rc)) {
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    while (!stopping) {
        output jobs[8];
        unsigned count = 0;
        uint64_t now = now_ns(), deadline = 0;
        for (unsigned i = 0; i < 8; ++i) {
            slot *s = &slots[i];
            if (live[i].generation != s->target.generation) {
                if (live[i].count) {
                    jobs[count++] = (output){.target = live[i], .slot = i, .stop = true};
                    continue; /* old device must acknowledge zero before rebinding */
                }
                live[i] = s->target;
            }
            sl_haptic_frame frame;
            if (live[i].count && sl_haptic_advance(&s->stream, now, &frame))
                jobs[count++] = (output){.target = live[i], .frame = frame, .slot = i};
            uint64_t next = live[i].count ? sl_haptic_deadline(&s->stream) : 0;
            if (next && (!deadline || next < deadline))
                deadline = next;
        }
        if (count) {
            bool failed = false;
            pthread_mutex_unlock(&lock);
            for (unsigned i = 0; i < count; ++i) {
                Result sent = emit(&io, &jobs[i]);
                failed |= R_FAILED(sent);
                if (jobs[i].stop && R_SUCCEEDED(sent))
                    live[jobs[i].slot].count = 0;
            }
            if (failed) {
                struct timespec retry = {.tv_nsec = 1000000};
                nanosleep(&retry, NULL);
            }
            pthread_mutex_lock(&lock);
            continue;
        }
        if (deadline) {
            struct timespec until = {.tv_sec = (time_t)(deadline / 1000000000ull),
                                     .tv_nsec = (long)(deadline % 1000000000ull)};
            pthread_cond_timedwait(&wake, &lock, &until);
        } else {
            pthread_cond_wait(&wake, &lock);
        }
    }
    pthread_mutex_unlock(&lock);
    /* Bounded stop retries; release this connection only after all outputs have
     * been told to stop. Failures remain in the machine-readable snapshot. */
    for (unsigned i = 0; i < 8; ++i) {
        if (!live[i].count)
            continue;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            output stop = {.target = live[i], .slot = i, .stop = true};
            if (R_SUCCEEDED(emit(&io, &stop)))
                break;
        }
    }
    io_close(&io);
    pthread_mutex_lock(&lock);
    stats.worker_running = false;
    pthread_mutex_unlock(&lock);
    return NULL;
}

bool sl_rumble_init(void) {
    if (initialized)
        return available;
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0)
        return false;
    int rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc)
        rc = pthread_cond_init(&wake, &attr);
    pthread_condattr_destroy(&attr);
    if (rc)
        return false;
    pthread_mutex_lock(&lock);
    memset(&stats, 0, sizeof(stats));
    memset(slots, 0, sizeof(slots));
    for (unsigned i = 0; i < 8; ++i)
        sl_haptic_init(&slots[i].stream);
#if NSL_DIAGNOSTICS || defined(NSL_RUMBLE_TEST)
    trace_head = trace_count = 0;
#endif
#ifndef NSL_RUMBLE_TEST
    resource_id = appletGetAppletResourceUserId();
#endif
    stopping = ready = false;
    rc = pthread_create(&worker, NULL, run, NULL);
    if (rc) {
        pthread_mutex_unlock(&lock);
        pthread_cond_destroy(&wake);
        return false;
    }
    initialized = true;
    ++stats.worker_starts;
    while (!ready)
        pthread_cond_wait(&wake, &lock);
    available = stats.worker_running;
    pthread_mutex_unlock(&lock);
    return available;
}
void sl_rumble_shutdown(void) {
    if (!initialized)
        return;
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    pthread_mutex_lock(&lock);
    ++stats.worker_joins;
    for (unsigned i = 0; i < 8; ++i) {
        sl_haptic_stop(&slots[i].stream, now_ns());
        slots[i].target.count = 0;
    }
    pthread_mutex_unlock(&lock);
    pthread_cond_destroy(&wake);
    initialized = available = false;
}

static u32 location(int index, HidNpadIdType *id) {
    *id = (HidNpadIdType)(HidNpadIdType_No1 + index);
    if (index == 0 && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld))
        *id = HidNpadIdType_Handheld;
    return hidGetNpadStyleSet(*id);
}
int sl_rumble_command(int32_t index, const uint8_t *command, size_t size,
                      uint64_t received, void *unused) {
    (void)unused;
    if (index < 0 || index >= 8 || !SDL_JoystickFromInstanceID(index))
        return SDL_SetError("Unknown Switch vibration controller");
    if (!initialized || !available)
        return SDL_SetError("Switch vibration worker unavailable");
    if (!sl_haptic_valid_command(command, size)) {
        pthread_mutex_lock(&lock);
        ++slots[index].stream.stats.rejected;
        pthread_mutex_unlock(&lock);
        return SDL_SetError("Invalid Switch vibration command");
    }
    HidNpadIdType id;
    u32 style = location(index, &id);
    unsigned count = style & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual |
                              HidNpadStyleTag_NpadFullKey) ? 2 :
        style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight) ? 1 : 0;
    if (!count)
        return SDL_SetError("No native vibration actuator");
    pthread_mutex_lock(&lock);
    binding target = slots[index].target;
    pthread_mutex_unlock(&lock);
    if (!target.count || target.id != id || target.style != style) {
        target = (binding){.id = id, .style = style, .count = count,
                           .generation = target.generation + 1};
        Result rc = hidInitializeVibrationDevices(target.handles, (int)count, id,
                                                   (HidNpadStyleTag)style);
        if (R_FAILED(rc))
            return SDL_SetError("Switch vibration activation failed: %08x", (unsigned)rc);
    }
    uint64_t now = now_ns();
    pthread_mutex_lock(&lock);
    slot *s = &slots[index];
    if (s->target.generation != target.generation) {
        sl_haptic_stop(&s->stream, now);
        /* The worker zeros the old binding before switching generations.
         * The fresh binding starts with this command, without a synthetic
         * ordinary-output frame in front of its native samples. */
        s->stream.dirty = false;
        s->target = target;
    }
    bool ok = sl_haptic_command(&s->stream, command, size, received, now,
        (style & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual)) != 0);
    sl_rumble_trace t = {.kind = 1, .slot = (unsigned)index, .received_ns = received,
        .submitted_ns = now, .sequence = s->stream.stats.commands,
        .size = size <= 69 ? (unsigned)size : 0, .result = ok ? 0 : UINT32_MAX};
    if (command && t.size)
        memcpy(t.command, command, t.size);
    record(&t);
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    return ok ? 0 : SDL_SetError("Invalid or overflowing Switch vibration command");
}
int __wrap_SDL_GameControllerRumble(SDL_GameController *controller, Uint16 low, Uint16 high,
                                    Uint32 duration_ms) {
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(controller);
    int32_t index = joy ? SDL_JoystickInstanceID(joy) : -1;
    uint8_t command[9] = {1, low & 255, low >> 8, high & 255, high >> 8,
        duration_ms & 255, (duration_ms >> 8) & 255, (duration_ms >> 16) & 255,
        duration_ms >> 24};
    return sl_rumble_command(index, command, sizeof(command), now_ns(), NULL);
}
void sl_rumble_tick(bool active) {
    if (!initialized)
        return;
    for (unsigned i = 0; i < 8; ++i) {
        HidNpadIdType id;
        u32 style = location((int)i, &id);
        bool connected = SDL_JoystickFromInstanceID((int)i) != NULL;
        pthread_mutex_lock(&lock);
        slot *s = &slots[i];
        if (s->target.count && (!active || !connected || s->target.id != id ||
                               s->target.style != style)) {
            sl_haptic_stop(&s->stream, now_ns());
            s->target.count = 0;
            ++s->target.generation;
            pthread_cond_signal(&wake);
        }
        pthread_mutex_unlock(&lock);
    }
}
void sl_rumble_get_snapshot(sl_rumble_snapshot *out) {
    pthread_mutex_lock(&lock);
    *out = stats;
    for (unsigned i = 0; i < 8; ++i)
        out->stream[i] = slots[i].stream.stats;
    pthread_mutex_unlock(&lock);
}
size_t sl_rumble_copy_trace(sl_rumble_trace *out, size_t capacity) {
    return sl_rumble_read_trace(0, out, capacity);
}
size_t sl_rumble_read_trace(uint64_t after_serial, sl_rumble_trace *out, size_t capacity) {
    size_t count = 0;
#if NSL_DIAGNOSTICS || defined(NSL_RUMBLE_TEST)
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < trace_count && count < capacity; ++i) {
        const sl_rumble_trace *r = &trace[(trace_head + TRACE_CAP - trace_count + i) % TRACE_CAP];
        if (r->serial > after_serial)
            out[count++] = *r;
    }
    pthread_mutex_unlock(&lock);
#else
    (void)out;
    (void)capacity;
    (void)after_serial;
#endif
    return count;
}
