/*
 * Copyright (C) 2014 Codership Oy <info@codership.com>
 */

#include "gu_assert.hpp"
#include "stats.hpp"

static const char* stats_key_string[] = {
    "msg_repl_latency",
    0
};

const char*
gcomm::stats_key_to_string(gcomm::StatsKey key)
{
    assert(key < gcomm::S_MAX);
    return stats_key_string[key];
}
