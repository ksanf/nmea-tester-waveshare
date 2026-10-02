/* Copyright (c) 2026 S. Zhurba. SPDX-License-Identifier: MIT */
#pragma once
void profile_test_log(const char *tag, const char *format, ...);
#define HLOGI(...) profile_test_log(__VA_ARGS__)
