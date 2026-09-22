#include "sysdvr/session_manager.h"
#include "sysdvr/log.h"

#include <stdio.h>
#include <string.h>

static bool same_device(
    const session_device *entry,
    const sysdvr_device *device)
{
    if (entry->info.serial[0] != '\0' &&
        device->serial[0] != '\0') {
        return strcmp(entry->info.serial, device->serial) == 0;
    }

    return strcmp(entry->info.host, device->host) == 0;
}

void session_manager_init(session_manager *m)
{
    memset(m, 0, sizeof(*m));
}

void session_manager_seen(
    session_manager *m,
    const sysdvr_device *device,
    uint64_t now_ms)
{
    if (m == NULL || device == NULL) {
        return;
    }

    for (size_t i = 0; i < m->count; ++i) {
        if (same_device(&m->devices[i], device)) {
            m->devices[i].info = *device;
            m->devices[i].last_seen_ms = now_ms;
            m->devices[i].manual = false;
            return;
        }
    }

    if (m->count >= SYSDVR_SESSION_MAX_DEVICES) {
        return;
    }

    session_device *entry = &m->devices[m->count++];
    memset(entry, 0, sizeof(*entry));
    entry->info = *device;
    entry->last_seen_ms = now_ms;

    LOGF(
            "[session] discovered device host=%s serial=%s version=%s protocol=%s\n",
            device->host,
            device->serial[0] ? device->serial : "?",
            device->client_version,
            device->protocol);
}

void session_manager_add_manual(
    session_manager *m,
    const char *host)
{
    if (m == NULL || host == NULL || host[0] == '\0' ||
        m->count >= SYSDVR_SESSION_MAX_DEVICES) {
        return;
    }

    session_device *entry = &m->devices[m->count++];
    memset(entry, 0, sizeof(*entry));

    snprintf(entry->info.host, sizeof(entry->info.host), "%s", host);
    snprintf(entry->info.client_version,
             sizeof(entry->info.client_version),
             "%s",
             "manual");
    snprintf(entry->info.protocol,
             sizeof(entry->info.protocol),
             "%s",
             "--");
    entry->manual = true;
    entry->last_seen_ms = UINT64_MAX;

    LOGF("[session] manual device entry host=%s\n", host);
}

void session_manager_prune(
    session_manager *m,
    uint64_t now_ms)
{
    if (m == NULL) {
        return;
    }

    sysdvr_device selected_info;
    bool had_selection =
        m->count > 0 &&
        m->selected < m->count;

    if (had_selection) {
        selected_info =
            m->devices[m->selected].info;
    } else {
        memset(&selected_info, 0, sizeof(selected_info));
    }

    size_t write_index = 0;

    for (size_t i = 0; i < m->count; ++i) {
        session_device *entry =
            &m->devices[i];

        bool keep =
            entry->manual ||
            now_ms < entry->last_seen_ms ||
            now_ms - entry->last_seen_ms
                <= SYSDVR_DEVICE_STALE_MS;

        if (keep) {
            if (write_index != i) {
                m->devices[write_index] =
                    m->devices[i];
            }
            ++write_index;
        } else {
            uint64_t age_ms =
                now_ms >= entry->last_seen_ms
                    ? now_ms - entry->last_seen_ms
                    : 0;

            LOGF(
                "[session] device expired host=%s serial=%s age_ms=%llu\n",
                entry->info.host,
                entry->info.serial[0] ? entry->info.serial : "?",
                (unsigned long long)age_ms
            );
        }
    }

    m->count = write_index;

    if (m->count == 0) {
        m->selected = 0;
        return;
    }

    if (had_selection) {
        for (size_t i = 0; i < m->count; ++i) {
            if (same_device(
                    &m->devices[i],
                    &selected_info)) {
                m->selected = i;
                return;
            }
        }
    }

    if (m->selected >= m->count) {
        m->selected = m->count - 1;
    }
}

void session_manager_move(
    session_manager *m,
    int delta)
{
    if (m == NULL || m->count == 0 || delta == 0) {
        return;
    }

    int selected = (int)m->selected + delta;

    if (selected < 0) {
        selected = (int)m->count - 1;
    } else if (selected >= (int)m->count) {
        selected = 0;
    }

    size_t old_selected = m->selected;
    m->selected = (size_t)selected;

    LOGF(
        "[input] selection moved old=%zu new=%zu count=%zu\n",
        old_selected,
        m->selected,
        m->count
    );
}

const session_device *session_manager_selected(
    const session_manager *m)
{
    if (m == NULL || m->count == 0 || m->selected >= m->count) {
        return NULL;
    }

    return &m->devices[m->selected];
}

void session_manager_remember_selected(
    session_manager *m)
{
    const session_device *entry = session_manager_selected(m);
    if (entry == NULL) {
        return;
    }

    snprintf(
        m->preferred_host,
        sizeof(m->preferred_host),
        "%s",
        entry->info.host
    );

    snprintf(
        m->preferred_serial,
        sizeof(m->preferred_serial),
        "%s",
        entry->info.serial
    );

    m->have_preferred = true;

    LOGF(
            "[session] preferred host=%s serial=%s\n",
            m->preferred_host,
            m->preferred_serial[0] ? m->preferred_serial : "?");
}

bool session_manager_matches_preferred(
    const session_manager *m,
    const sysdvr_device *device)
{
    if (m == NULL || device == NULL || !m->have_preferred) {
        return false;
    }

    if (m->preferred_serial[0] != '\0' &&
        device->serial[0] != '\0') {
        return strcmp(m->preferred_serial, device->serial) == 0;
    }

    return strcmp(m->preferred_host, device->host) == 0;
}
