/** @file
 *  @brief CTS Service sample
 */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __cplusplus
extern "C" {
#endif

void cts_init(void);
int cts_notify(struct bt_conn *conn);

#ifdef __cplusplus
}
#endif
