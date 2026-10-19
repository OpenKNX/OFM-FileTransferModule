/**
 * @file        HttpLib.h
 * @brief       cpp-httplib for oknx: the one place that includes it.
 * @details     On Windows its <sys/stat.h> defines `stat` as a macro (_stat64), which renames every later member
 *              called `stat` (MonFrame::stat among them). httplib itself is preprocessed by then, so the macro
 *              is dropped right after it.
 * @date        2026-10-19
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 **/
#pragma once
#include "third_party/httplib.h"

#ifdef stat
    #undef stat
#endif
